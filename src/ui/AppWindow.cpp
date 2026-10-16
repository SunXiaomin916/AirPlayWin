#include "ui/AppWindow.h"

#include <objbase.h>
#include <WebView2.h>
#include <wrl.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "AirPlayWinVersion.h"
#include "app/PlatformCommands.h"
#include "app/TestTonePlayer.h"

namespace airplaywin::ui {

using airplaywin::app::EnumerateAudioDevices;
using airplaywin::app::EnumerateNetworkInterfaces;
using airplaywin::app::PlayTestTone;
using airplaywin::app::ReceiverHost;
using airplaywin::app::ReceiverHostConfig;
using airplaywin::app::ReceiverSnapshot;
using airplaywin::app::RunEndpointCalibrationCommand;
using airplaywin::app::RunFirewallCommand;
using airplaywin::app::SettingsStore;
using airplaywin::app::TestToneRequest;

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr UINT kMessageTrayIcon = WM_APP + 2U;
constexpr UINT kMessagePushState = WM_APP + 3U;
constexpr UINT kMessageToneFinished = WM_APP + 4U;
constexpr UINT kMessageReceiverState = WM_APP + 5U;
constexpr UINT kMessageStartFinished = WM_APP + 6U;
constexpr UINT kMessageStopFinished = WM_APP + 7U;

constexpr UINT kTrayCommandShow = 1U;
constexpr UINT kTrayCommandToggle = 2U;
constexpr UINT kTrayCommandExit = 3U;
constexpr UINT kTrayIconId = 1U;

void AppendJsonString(std::string& out, const std::string_view value) {
    out.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[byte >> 4U]);
                out.push_back(hex[byte & 0x0FU]);
            } else {
                out.push_back(character);
            }
            break;
        }
    }
    out.push_back('"');
}

void AppendJsonString(std::string& out, const std::wstring_view value) {
    if (value.empty()) {
        out += "\"\"";
        return;
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (required <= 0) {
        out += "\"\"";
        return;
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    static_cast<void>(WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()), utf8.data(),
                                          required, nullptr, nullptr));
    AppendJsonString(out, utf8);
}

[[nodiscard]] std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int required =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    static_cast<void>(MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), result.data(),
                                          required));
    return result;
}

[[nodiscard]] std::wstring ExecutablePath() {
    std::wstring path(32'767U, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(),
                                           static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size()) {
        return {};
    }
    path.resize(length);
    return path;
}

[[nodiscard]] std::wstring AutostartCommand() {
    return L"\"" + ExecutablePath() + L"\" --minimized";
}

[[nodiscard]] bool ReadAutostart() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[32'768U]{};
    DWORD bytes = sizeof(value);
    const auto result = RegQueryValueExW(key, L"AirPlayWin", nullptr, nullptr,
                                         reinterpret_cast<BYTE*>(value), &bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && value[0] != L'\0';
}

[[nodiscard]] bool WriteAutostart(const bool enabled) noexcept {
    const auto path = AutostartCommand();
    if (enabled) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER,
                            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                            nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                            nullptr) != ERROR_SUCCESS) {
            return false;
        }
        const auto bytes =
            static_cast<DWORD>((path.size() + 1U) * sizeof(wchar_t));
        const auto result =
            RegSetValueExW(key, L"AirPlayWin", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(path.c_str()), bytes);
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const auto result = RegDeleteValueW(key, L"AirPlayWin");
    RegCloseKey(key);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

[[nodiscard]] std::wstring MakeLogPath(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::tm now{};
    const auto clock = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(clock);
    localtime_s(&now, &time);
    wchar_t timestamp[64U]{};
    std::wcsftime(timestamp, 64U, L"%Y%m%d-%H%M%S", &now);
    return (directory / (std::wstring{L"app-"} + timestamp + L".log")).wstring();
}

[[nodiscard]] bool RedirectDiagnosticsToFile(const std::filesystem::path& directory) {
    const auto path = MakeLogPath(directory);
    FILE* output = nullptr;
    output = _wfsopen(path.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    if (output == nullptr) {
        return false;
    }
    const auto output_descriptor = _fileno(output);
    if (_dup2(output_descriptor, _fileno(stdout)) != 0 ||
        _dup2(output_descriptor, _fileno(stderr)) != 0 ||
        _setmode(_fileno(stdout), _O_U8TEXT) == -1 ||
        _setmode(_fileno(stderr), _O_U8TEXT) == -1) {
        static_cast<void>(std::fclose(output));
        return false;
    }
    static_cast<void>(std::fclose(output));
    std::wcout.clear();
    std::wcerr.clear();
    return true;
}

}  // namespace

class AppWindow::Impl final {
public:
    Impl(const HINSTANCE instance, AppSettings settings,
         const std::filesystem::path ui_directory,
         const std::filesystem::path log_directory, const bool test_bridge)
        : instance_(instance),
          settings_(std::move(settings)),
          ui_directory_(ui_directory),
          log_directory_(log_directory),
          settings_store_(log_directory.parent_path() / "settings.json"),
          receiver_(std::make_unique<ReceiverHost>(BuildReceiverConfig())),
          tone_thread_active_(false),
          test_bridge_(test_bridge) {
        static_cast<void>(RedirectDiagnosticsToFile(log_directory));
    }

    ~Impl() {
        receiver_->Stop();
        RemoveTrayIcon();
    }

    [[nodiscard]] bool Initialize(const int show_command) noexcept {
        RegisterWindowClass();
        const auto width = GetSystemMetrics(SM_CXSCREEN);
        const auto height = GetSystemMetrics(SM_CYSCREEN);
        const int window_width = std::min(1'160L, static_cast<LONG>(width * 3 / 4));
        const int window_height = std::min(760L, static_cast<LONG>(height * 3 / 4));
        const int x = (width - window_width) / 2;
        const int y = (height - window_height) / 2;
        hwnd_ = CreateWindowExW(
            0U, kClassName, L"AirPlayWin", WS_OVERLAPPEDWINDOW, x, y, window_width,
            window_height, nullptr, nullptr, instance_, this);
        if (hwnd_ == nullptr) {
            return false;
        }
        receiver_->SetDiagnosticsCallback([this](const ReceiverSnapshot&) {
            // The receiver thread marshals to the UI thread, which owns the
            // WebView2 controller.
            if (hwnd_ != nullptr) {
                PostMessageW(hwnd_, kMessagePushState, 0U, 0U);
            }
        });
        receiver_->SetStateCallback([this](const bool running, const std::uint32_t error,
                                          const std::wstring message) {
            if (hwnd_ != nullptr) {
                auto* payload = new ReceiverStateEvent{running, error, message};
                PostMessageW(hwnd_, kMessageReceiverState, 0U,
                             reinterpret_cast<LPARAM>(payload));
            }
        });
        CreateTrayIcon();
        StartWebView();
        ShowWindow(hwnd_, show_command);
        UpdateWindow(hwnd_);
        return true;
    }

    [[nodiscard]] int Run() noexcept {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0U, 0U) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

    void ShowWindowFromTray() noexcept {
        if (hwnd_ != nullptr) {
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
        }
    }

    void RequestExit() noexcept {
        if (hwnd_ != nullptr) {
            PostMessageW(hwnd_, WM_CLOSE, 0U, 0U);
        }
    }

private:
    struct ReceiverStateEvent final {
        bool running{false};
        std::uint32_t error{0U};
        std::wstring message{};
    };

    static constexpr wchar_t kClassName[] = L"AirPlayWinAppWindow";

    [[nodiscard]] ReceiverHostConfig BuildReceiverConfig() const noexcept {
        ReceiverHostConfig config;
        config.device_name = settings_.device_name;
        config.output_device_id = settings_.output_device_id;
        config.low_latency = settings_.low_latency;
        config.exclusive_audio = settings_.exclusive_audio;
        config.strict_exclusive = settings_.strict_exclusive;
        config.raop_port = settings_.raop_port;
        config.airplay_port = settings_.airplay_port;
        config.classic_raop = settings_.classic_raop;
        config.include_virtual_interfaces = settings_.include_virtual_interfaces;
        config.experimental_buffered_timing = settings_.experimental_buffered_timing;
        config.buffered_timing_milliseconds = settings_.buffered_timing_milliseconds;
        config.experimental_ptp_timing = settings_.experimental_ptp_timing;
        config.ptp_interface_address = settings_.ptp_interface_address;
        config.endpoint_calibration_offset_microseconds =
            settings_.endpoint_calibration_offset_microseconds;
        config.diagnostics_interval_seconds = settings_.diagnostics_interval_seconds;
        return config;
    }

    void RegisterWindowClass() noexcept {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &Impl::WindowProcedure;
        window_class.hInstance = instance_;
        window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        window_class.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kClassName;
        RegisterClassExW(&window_class);
    }

    static LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wparam,
                                            LPARAM lparam) noexcept {
        auto* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        return self->HandleMessage(hwnd, message, wparam, lparam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        switch (message) {
        case WM_SIZE:
            if (webview_controller_ != nullptr && wparam != SIZE_MINIMIZED) {
                RECT bounds{};
                GetClientRect(hwnd, &bounds);
                static_cast<void>(webview_controller_->put_Bounds(bounds));
            }
            return 0;
        case WM_CLOSE:
            if (settings_.minimize_to_tray) {
                HideToTray();
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon();
            webview_controller_.Reset();
            webview_.Reset();
            webview_environment_.Reset();
            {
                // Wait for any in-flight start/stop worker so it cannot
                // touch the receiver after destruction.
                std::scoped_lock lock{lifecycle_work_mutex_};
                receiver_->Stop();
            }
            PostQuitMessage(0);
            return 0;
        case kMessagePushState:
            PushStateToWebView();
            return 0;
        case kMessageReceiverState: {
            auto* payload = reinterpret_cast<ReceiverStateEvent*>(lparam);
            if (payload != nullptr) {
                std::string json{"{\"type\":\"receiver\",\"payload\":{"};
                json += "\"running\":" + std::string(payload->running ? "true" : "false");
                json += ",\"error\":" + std::to_string(payload->error);
                json += ",\"message\":";
                AppendJsonString(json, payload->message);
                json += "}}";
                PostToWebView(json);
                delete payload;
            }
            return 0;
        }
        case kMessageStartFinished:
        case kMessageStopFinished: {
            auto* payload = reinterpret_cast<LifecycleResult*>(lparam);
            if (payload != nullptr) {
                if (payload->ok) {
                    SendResponse(payload->response_id, "{\"ok\":true}", true);
                } else {
                    SendOkResponse(payload->response_id, false, payload->message);
                }
                delete payload;
            }
            PushStateToWebView();
            return 0;
        }
        case kMessageToneFinished:
            if (lparam != 0L) {
                auto* payload = reinterpret_cast<std::string*>(lparam);
                PostToWebView(*payload);
                delete payload;
                tone_thread_active_ = false;
            }
            return 0;
        case kMessageTrayIcon:
            if (lparam == WM_LBUTTONDBLCLK || lparam == WM_LBUTTONUP) {
                ShowWindowFromTray();
                return 0;
            }
            if (lparam == WM_RBUTTONUP) {
                ShowTrayMenu();
                return 0;
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case kTrayCommandShow:
                ShowWindowFromTray();
                return 0;
            case kTrayCommandToggle:
                if (receiver_->Running()) {
                    receiver_->Stop();
                } else {
                    StartReceiver();
                }
                return 0;
            case kTrayCommandExit:
                settings_.minimize_to_tray = false;
                static_cast<void>(settings_store_.Save(settings_));
                DestroyWindow(hwnd);
                return 0;
            default:
                break;
            }
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // ---- WebView2 ----

    void StartWebView() noexcept {
        const auto html_path = ui_directory_ / "index.html";
        std::error_code error;
        if (!std::filesystem::exists(html_path, error)) {
            MessageBoxW(hwnd_,
                        L"未找到界面资源文件 ui\\index.html。请从完整的安装包启动。",
                        L"AirPlayWin", MB_ICONERROR);
            return;
        }
        std::wstring user_data_folder =
            std::filesystem::temp_directory_path().wstring() + L"AirPlayWinWebView";
        auto* self = this;
        static_cast<void>(CreateCoreWebView2EnvironmentWithOptions(
            nullptr, user_data_folder.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [self](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                    if (FAILED(result) || environment == nullptr) {
                        std::wcerr << L"WebView2 environment creation failed: 0x" << std::hex
                                   << static_cast<std::uint32_t>(result) << std::dec << L"\n";
                        return result;
                    }
                    std::wcerr << L"WebView2 environment ready.\n";
                    self->webview_environment_ = environment;
                    return self->webview_environment_->CreateCoreWebView2Controller(
                        self->hwnd_,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [self](HRESULT controller_result,
                                   ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(controller_result) || controller == nullptr) {
                                    std::wcerr << L"WebView2 controller creation failed: 0x"
                                               << std::hex
                                               << static_cast<std::uint32_t>(controller_result)
                                               << std::dec << L"\n";
                                    return controller_result;
                                }
                                std::wcerr << L"WebView2 controller ready.\n";
                                self->OnWebViewControllerReady(controller);
                                return S_OK;
                            })
                            .Get());
                })
                .Get()));
    }

    void OnWebViewControllerReady(ICoreWebView2Controller* controller) noexcept {
        webview_controller_ = controller;
        static_cast<void>(webview_controller_->get_CoreWebView2(&webview_));
        if (webview_ == nullptr) {
            return;
        }
        RECT bounds{};
        GetClientRect(hwnd_, &bounds);
        static_cast<void>(webview_controller_->put_Bounds(bounds));
        ComPtr<ICoreWebView2Settings> settings;
        static_cast<void>(webview_->get_Settings(&settings));
        static_cast<void>(settings->put_AreDevToolsEnabled(FALSE));
        static_cast<void>(settings->put_IsStatusBarEnabled(FALSE));
        static_cast<void>(settings->put_IsZoomControlEnabled(TRUE));
        auto* self = this;
        static_cast<void>(webview_->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [self](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*
                                            arguments) -> HRESULT {
                    // The page posts a JSON string; TryGetWebMessageAsString
                    // returns it verbatim (AsJson would re-encode it as a
                    // JSON string literal with escaped quotes).
                    LPWSTR message = nullptr;
                    if (FAILED(arguments->TryGetWebMessageAsString(&message)) ||
                        message == nullptr) {
                        return S_OK;
                    }
                    self->HandleWebMessage(std::wstring{message});
                    CoTaskMemFree(message);
                    return S_OK;
                })
                .Get(),
            nullptr));
        const auto html_path = (ui_directory_ / "index.html").wstring();
        std::wstring file_url{L"file:///"};
        for (const wchar_t character : html_path) {
            if (character == L'\\') {
                file_url.push_back(L'/');
            } else {
                file_url.push_back(character);
            }
        }
        static_cast<void>(webview_->Navigate(file_url.c_str()));
        if (test_bridge_) {
            auto* navigation_self = this;
            static_cast<void>(webview_->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [navigation_self](ICoreWebView2*,
                                      ICoreWebView2NavigationCompletedEventArgs*
                                          arguments) -> HRESULT {
                        BOOL success = FALSE;
                        if (FAILED(arguments->get_IsSuccess(&success)) || !success) {
                            navigation_self->ReportBridgeTestError(
                                L"page navigation failed");
                            return S_OK;
                        }
                        std::wcerr << L"Navigation completed; starting bridge test.\n";
                        static_cast<void>(navigation_self->webview_->ExecuteScript(
                            L"window.__runBridgeTest ? window.__runBridgeTest() : "
                            L"window.chrome.webview.postMessage(JSON.stringify({id:0,"
                            L"command:'testResult',payload:{error:'__runBridgeTest "
                            L"missing'}}))",
                            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                                [navigation_self](HRESULT script_result, LPCWSTR) -> HRESULT {
                                    if (FAILED(script_result)) {
                                        navigation_self->ReportBridgeTestError(
                                            L"ExecuteScript failed: 0x" +
                                            std::to_wstring(
                                                static_cast<std::uint32_t>(script_result)));
                                    }
                                    return S_OK;
                                })
                                .Get()));
                        return S_OK;
                    })
                    .Get(),
                nullptr));
        }
        PushStateToWebView();
    }

    void ReportBridgeTestError(const std::wstring& message) noexcept {
        const auto result_path = log_directory_.parent_path() / "test-bridge.json";
        std::ofstream stream{result_path, std::ios::binary | std::ios::trunc};
        if (stream) {
            const auto utf8 = [&]() {
                const int required = WideCharToMultiByte(
                    CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0,
                    nullptr, nullptr);
                std::string result(static_cast<std::size_t>(required), '\0');
                static_cast<void>(WideCharToMultiByte(
                    CP_UTF8, 0, message.data(), static_cast<int>(message.size()),
                    result.data(), required, nullptr, nullptr));
                return result;
            }();
            stream << "{\"error\":\"" << utf8 << "\"}";
        }
        settings_.minimize_to_tray = false;
        static_cast<void>(settings_store_.Save(settings_));
        DestroyWindow(hwnd_);
    }

    void PostToWebView(const std::string_view json) noexcept {
        if (webview_ == nullptr) {
            return;
        }
        const auto wide = Utf8ToWide(json);
        static_cast<void>(webview_->PostWebMessageAsJson(wide.c_str()));
    }

    // ---- Message bridge ----

    struct Message final {
        std::uint64_t id{0U};
        std::wstring command{};
        std::string payload{};
    };

    struct LifecycleResult final {
        std::uint64_t response_id{0U};
        bool ok{false};
        std::wstring message{};
    };

    void HandleWebMessage(const std::wstring& message_json) noexcept {
        // The UI sends {"id": n, "command": "...", "payload": {...}}. The
        // payload is re-serialized verbatim by the JavaScript side, so the
        // command handlers parse it with the same minimal JSON approach.
        Message message;
        if (!ParseMessage(message_json, message)) {
            std::wcerr << L"Web message parse failed: "
                       << message_json.substr(0U, std::min<std::size_t>(120U, message_json.size()))
                       << L"\n";
            return;
        }
        std::wcerr << L"Web message: " << message.command << L"\n";
        if (message.command == L"getState") {
            SendStateResponse(message.id);
        } else if (message.command == L"start") {
            RunStartAsync(message.id);
        } else if (message.command == L"stop") {
            RunStopAsync(message.id);
        } else if (message.command == L"setConfig") {
            ApplyConfigPayload(message.payload);
            SendResponse(message.id, "{\"ok\":true}", true);
        } else if (message.command == L"listDevices") {
            SendResponse(message.id, DevicesJson(), false);
        } else if (message.command == L"listInterfaces") {
            SendResponse(message.id, InterfacesJson(), false);
        } else if (message.command == L"firewallStatus") {
            const auto result = RunFirewallCommand(
                ExecutablePath(), settings_.raop_port, settings_.airplay_port,
                settings_.classic_raop, false, false, true);
            std::string json{"{\"ok\":"};
            json += result.succeeded ? "true" : "false";
            json += ",\"detail\":";
            AppendJsonString(json, result.detail);
            json += "}";
            SendResponse(message.id, json, true);
        } else if (message.command == L"installFirewall") {
            ElevateForFirewall(L"--install-firewall-rules");
            SendResponse(message.id, "{\"ok\":true}", true);
        } else if (message.command == L"removeFirewall") {
            ElevateForFirewall(L"--remove-firewall-rules");
            SendResponse(message.id, "{\"ok\":true}", true);
        } else if (message.command == L"playTestTone") {
            StartTestTone(message.id, message.payload);
        } else if (message.command == L"getCalibration") {
            const auto device = ReadStringField(message.payload, L"deviceId");
            const auto result = RunEndpointCalibrationCommand(
                false, true, false, device, 0);
            std::string response{"{\"ok\":"};
            response += result.succeeded ? "true" : "false";
            if (result.offset_microseconds.has_value()) {
                response += ",\"offsetUs\":" +
                            std::to_string(*result.offset_microseconds);
            }
            response += "}";
            SendResponse(message.id, response, true);
        } else if (message.command == L"saveCalibration") {
            const auto device = ReadStringField(message.payload, L"deviceId");
            const auto offset = ReadIntField(message.payload, L"offsetUs");
            const auto result =
                RunEndpointCalibrationCommand(true, false, false, device, offset);
            SendOkResponse(message.id, result.succeeded, result.detail);
        } else if (message.command == L"clearCalibration") {
            const auto device = ReadStringField(message.payload, L"deviceId");
            const auto result =
                RunEndpointCalibrationCommand(false, false, true, device, 0);
            SendOkResponse(message.id, result.succeeded, result.detail);
        } else if (message.command == L"getAutostart") {
            std::string response{"{\"enabled\":"};
            response += ReadAutostart() ? "true" : "false";
            response += "}";
            SendResponse(message.id, response, true);
        } else if (message.command == L"setAutostart") {
            const auto enabled = ReadBoolField(message.payload, L"enabled");
            const bool applied = WriteAutostart(enabled);
            settings_.autostart = enabled;
            static_cast<void>(settings_store_.Save(settings_));
            std::string response{"{\"ok\":"};
            response += applied ? "true" : "false";
            response += "}";
            SendResponse(message.id, response, true);
        } else if (message.command == L"setVolume") {
            const auto gain = ReadFloatField(message.payload, L"gain");
            std::wcerr << L"setVolume: parsed gain=" << gain << L" payload="
                       << Utf8ToWide(message.payload).substr(
                              0U, std::min<std::size_t>(60U, message.payload.size()))
                       << L"\n";
            settings_.volume = std::clamp(gain, 0.0F, 1.0F);
            static_cast<void>(receiver_->SetVolume(settings_.volume));
            static_cast<void>(settings_store_.Save(settings_));
            SendResponse(message.id, "{\"ok\":true}", true);
        } else if (message.command == L"openLogsFolder") {
            static_cast<void>(ShellExecuteW(nullptr, L"open", log_directory_.c_str(),
                                            nullptr, nullptr, SW_SHOWNORMAL));
            SendResponse(message.id, "{\"ok\":true}", true);
        } else if (message.command == L"hideToTray") {
            HideToTray();
        } else if (message.command == L"testResult") {
            // Automated bridge test mode: persist the result and close.
            const auto result_path = log_directory_.parent_path() / "test-bridge.json";
            std::ofstream stream{result_path, std::ios::binary | std::ios::trunc};
            if (stream) {
                stream.write(message.payload.data(),
                             static_cast<std::streamsize>(message.payload.size()));
            }
            settings_.minimize_to_tray = false;
            static_cast<void>(settings_store_.Save(settings_));
            DestroyWindow(hwnd_);
        } else if (message.command == L"exitApp") {
            settings_.minimize_to_tray = false;
            static_cast<void>(settings_store_.Save(settings_));
            DestroyWindow(hwnd_);
        }
    }

    [[nodiscard]] bool ParseMessage(const std::wstring& json, Message& message) noexcept {
        // Minimal scan for {"id":N,"command":"...","payload":...}.
        const auto find = [&](const std::wstring_view key, std::size_t from) {
            return json.find(key, from);
        };
        const auto id_position = find(L"\"id\"", 0U);
        if (id_position != std::wstring::npos) {
            const auto colon = json.find(L':', id_position);
            if (colon != std::wstring::npos) {
                try {
                    message.id = static_cast<std::uint64_t>(
                        std::stoull(json.substr(colon + 1U)));
                } catch (...) {
                    message.id = 0U;
                }
            }
        }
        const auto command_position = find(L"\"command\"", 0U);
        if (command_position == std::wstring::npos) {
            std::wcerr << L"ParseMessage: command key not found\n";
            return false;
        }
        const auto open = json.find(L'"', command_position + 10U);
        if (open == std::wstring::npos) {
            std::wcerr << L"ParseMessage: open quote not found at "
                       << (command_position + 10U) << L"\n";
            return false;
        }
        const auto close = json.find(L'"', open + 1U);
        if (close == std::wstring::npos) {
            std::wcerr << L"ParseMessage: close quote not found\n";
            return false;
        }
        message.command = json.substr(open + 1U, close - open - 1U);
        const auto payload_position = find(L"\"payload\"", close);
        if (payload_position != std::wstring::npos) {
            const auto colon = json.find(L':', payload_position);
            if (colon != std::wstring::npos) {
                const auto text = json.substr(colon + 1U);
                message.payload = narrow(text);
            }
        }
        return true;
    }

    [[nodiscard]] std::string narrow(const std::wstring_view text) const noexcept {
        const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                                 static_cast<int>(text.size()), nullptr, 0,
                                                 nullptr, nullptr);
        if (required <= 0) {
            return {};
        }
        std::string result(static_cast<std::size_t>(required), '\0');
        static_cast<void>(WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                              static_cast<int>(text.size()),
                                              result.data(), required, nullptr, nullptr));
        return result;
    }

    [[nodiscard]] std::wstring ReadStringField(const std::string& payload,
                                               const std::wstring_view key) const noexcept {
        const auto key_text = narrow(key);
        const auto position = payload.find("\"" + key_text + "\"");
        if (position == std::string::npos) {
            return {};
        }
        const auto colon = payload.find(':', position + key_text.size());
        if (colon == std::string::npos) {
            return {};
        }
        std::size_t value_begin = colon + 1U;
        while (value_begin < payload.size() &&
               (payload[value_begin] == ' ' || payload[value_begin] == '\t')) {
            ++value_begin;
        }
        if (value_begin >= payload.size()) {
            return {};
        }
        if (payload[value_begin] == '"') {
            const auto close = payload.find('"', value_begin + 1U);
            if (close == std::string::npos) {
                return {};
            }
            return Utf8ToWide(payload.substr(value_begin + 1U, close - value_begin - 1U));
        }
        // Numeric and boolean values are unquoted; read up to the next
        // separator.
        const auto end_position = payload.find_first_of(",}", value_begin);
        const auto end =
            end_position == std::string::npos ? payload.size() : end_position;
        return Utf8ToWide(payload.substr(value_begin, end - value_begin));
    }

    [[nodiscard]] std::int64_t ReadIntField(const std::string& payload,
                                            const std::wstring_view key) const noexcept {
        const auto text = ReadStringField(payload, key);
        try {
            return static_cast<std::int64_t>(std::stoll(text));
        } catch (...) {
            return 0;
        }
    }

    [[nodiscard]] float ReadFloatField(const std::string& payload,
                                       const std::wstring_view key) const noexcept {
        const auto text = ReadStringField(payload, key);
        try {
            return std::stof(text);
        } catch (...) {
            return 0.0F;
        }
    }

    [[nodiscard]] bool ReadBoolField(const std::string& payload,
                                     const std::wstring_view key) const noexcept {
        const auto text = ReadStringField(payload, key);
        return text == L"true" || text == L"1";
    }

    void SendResponse(const std::uint64_t id, const std::string& result_json,
                      const bool parse_as_plain) noexcept {
        std::string json{"{\"type\":\"response\",\"id\":"};
        json += std::to_string(id);
        json += ",\"ok\":true,\"result\":";
        if (parse_as_plain) {
            json += result_json;
        } else {
            // Devices/interfaces responses arrive pre-serialized.
            json += result_json;
        }
        json += "}";
        PostToWebView(json);
    }

    void SendOkResponse(const std::uint64_t id, const bool ok,
                        const std::wstring& detail) noexcept {
        std::string json{"{\"type\":\"response\",\"id\":"};
        json += std::to_string(id);
        json += ",\"ok\":";
        json += ok ? "true" : "false";
        json += ",\"detail\":";
        AppendJsonString(json, detail);
        json += "}";
        PostToWebView(json);
    }

    // ---- Receiver control ----

    // Receiver start/stop runs on a worker thread: WASAPI device open and
    // DNS-SD registration can block for seconds and must never stall the
    // UI message loop. Results marshal back to the UI thread.
    void RunStartAsync(const std::uint64_t response_id) {
        std::thread{[this, response_id]() {
            std::scoped_lock lock{lifecycle_work_mutex_};
            std::wcerr << L"Lifecycle worker: starting receiver...\n";
            bool ok = false;
            std::wstring message;
            if (!receiver_->Running()) {
                receiver_ = std::make_unique<ReceiverHost>(BuildReceiverConfig());
                receiver_->SetDiagnosticsCallback([this](const ReceiverSnapshot&) {
                    if (hwnd_ != nullptr) {
                        PostMessageW(hwnd_, kMessagePushState, 0U, 0U);
                    }
                });
                receiver_->SetStateCallback([this](const bool running,
                                                   const std::uint32_t error,
                                                   const std::wstring detail) {
                    if (hwnd_ != nullptr) {
                        auto* payload = new ReceiverStateEvent{running, error, detail};
                        PostMessageW(hwnd_, kMessageReceiverState, 0U,
                                     reinterpret_cast<LPARAM>(payload));
                    }
                });
                ok = receiver_->Start();
                std::wcerr << L"Lifecycle worker: Start returned " << (ok ? L"ok" : L"failed")
                           << L", error=" << receiver_->LastError() << L"\n";
                if (!ok) {
                    message = L"接收器启动失败（错误码 " +
                              std::to_wstring(receiver_->LastError()) +
                              L"）。请检查端口是否被占用、网络配置和防火墙设置。";
                }
            } else {
                ok = true;
            }
            std::wcerr << L"Lifecycle worker: posting result.\n";
            auto* payload = new LifecycleResult{response_id, ok, std::move(message)};
            PostMessageW(hwnd_, kMessageStartFinished, 0U,
                         reinterpret_cast<LPARAM>(payload));
        }}.detach();
    }

    void RunStopAsync(const std::uint64_t response_id) {
        std::thread{[this, response_id]() {
            std::scoped_lock lock{lifecycle_work_mutex_};
            receiver_->Stop();
            auto* payload = new LifecycleResult{response_id, true, {}};
            PostMessageW(hwnd_, kMessageStopFinished, 0U,
                         reinterpret_cast<LPARAM>(payload));
        }}.detach();
    }

    void StartReceiver() noexcept {
        if (receiver_->Running()) {
            return;
        }
        receiver_ = std::make_unique<ReceiverHost>(BuildReceiverConfig());
        receiver_->SetDiagnosticsCallback([this](const ReceiverSnapshot&) {
            if (hwnd_ != nullptr) {
                PostMessageW(hwnd_, kMessagePushState, 0U, 0U);
            }
        });
        receiver_->SetStateCallback([this](const bool running, const std::uint32_t error,
                                          const std::wstring message) {
            if (hwnd_ != nullptr) {
                auto* payload = new ReceiverStateEvent{running, error, message};
                PostMessageW(hwnd_, kMessageReceiverState, 0U,
                             reinterpret_cast<LPARAM>(payload));
            }
        });
        if (!receiver_->Start()) {
            auto* payload = new ReceiverStateEvent{
                false, receiver_->LastError(),
                L"接收器启动失败（错误码 " + std::to_wstring(receiver_->LastError()) +
                    L"）。请检查端口是否被占用、网络配置和防火墙设置。"};
            PostMessageW(hwnd_, kMessageReceiverState, 0U,
                         reinterpret_cast<LPARAM>(payload));
        }
        PushStateToWebView();
    }

    // ---- State serialization ----

    void PushStateToWebView() noexcept {
        if (webview_ == nullptr) {
            return;
        }
        const auto snapshot = receiver_->Snapshot();
        std::string json{"{\"type\":\"state\",\"payload\":{"};
        json += "\"version\":";
        AppendJsonString(json, AIRPLAYWIN_VERSION_STRING_W);
        json += ",\"running\":" + std::string(snapshot.running ? "true" : "false");
        json += ",\"lastError\":" + std::to_string(snapshot.last_error);
        json += ",\"settings\":";
        AppendSettingsJson(json);
        json += ",\"receiver\":";
        AppendReceiverJson(json, snapshot);
        json += "}}";
        PostToWebView(json);
    }

    void SendStateResponse(const std::uint64_t id) noexcept {
        const auto snapshot = receiver_->Snapshot();
        std::string json{"{\"type\":\"response\",\"id\":"};
        json += std::to_string(id);
        json += ",\"ok\":true,\"result\":{\"version\":";
        AppendJsonString(json, AIRPLAYWIN_VERSION_STRING_W);
        json += ",\"running\":" + std::string(snapshot.running ? "true" : "false");
        json += ",\"lastError\":" + std::to_string(snapshot.last_error);
        json += ",\"settings\":";
        AppendSettingsJson(json);
        json += ",\"receiver\":";
        AppendReceiverJson(json, snapshot);
        json += "}}";
        PostToWebView(json);
    }

    void AppendSettingsJson(std::string& json) const noexcept {
        json += "{";
        json += "\"deviceName\":";
        AppendJsonString(json, settings_.device_name);
        json += ",\"outputDeviceId\":";
        AppendJsonString(json, settings_.output_device_id);
        json += ",\"followDefaultDevice\":" +
                std::string(settings_.follow_default_device ? "true" : "false");
        json += ",\"lowLatency\":" + std::string(settings_.low_latency ? "true" : "false");
        json += ",\"exclusiveAudio\":" +
                std::string(settings_.exclusive_audio ? "true" : "false");
        json += ",\"strictExclusive\":" +
                std::string(settings_.strict_exclusive ? "true" : "false");
        json += ",\"raopPort\":" + std::to_string(settings_.raop_port);
        json += ",\"airplayPort\":" + std::to_string(settings_.airplay_port);
        json += ",\"classicRaop\":" + std::string(settings_.classic_raop ? "true" : "false");
        json += ",\"includeVirtualInterfaces\":" +
                std::string(settings_.include_virtual_interfaces ? "true" : "false");
        json += ",\"experimentalBufferedTiming\":" +
                std::string(settings_.experimental_buffered_timing ? "true" : "false");
        json += ",\"bufferedTimingMilliseconds\":" +
                std::to_string(settings_.buffered_timing_milliseconds);
        json += ",\"experimentalPtpTiming\":" +
                std::string(settings_.experimental_ptp_timing ? "true" : "false");
        json += ",\"ptpInterfaceAddress\":";
        AppendJsonString(json, settings_.ptp_interface_address);
        json += ",\"endpointCalibrationOffsetMicroseconds\":" +
                std::to_string(settings_.endpoint_calibration_offset_microseconds);
        json += ",\"volume\":" + std::to_string(settings_.volume);
        json += ",\"minimizeToTray\":" +
                std::string(settings_.minimize_to_tray ? "true" : "false");
        json += ",\"diagnosticsIntervalSeconds\":" +
                std::to_string(settings_.diagnostics_interval_seconds);
        json += "}";
    }

    void AppendReceiverJson(std::string& json,
                            const ReceiverSnapshot& snapshot) const noexcept {
        const auto& control = snapshot.control;
        const auto& transport = snapshot.transport;
        const auto& audio = snapshot.audio;
        const auto& output = audio.output;
        const auto& discovery = snapshot.discovery;
        json += "{";
        json += "\"advertisedName\":";
        AppendJsonString(json, discovery.advertised_name);
        json += ",\"sessionCount\":" + std::to_string(control.sessions.active_connections);
        json += ",\"receivedRequests\":" + std::to_string(control.received_requests);
        json += ",\"rejectedRequests\":" + std::to_string(control.rejected_requests);
        json += ",\"parseErrors\":" + std::to_string(control.parse_errors);
        json += ",\"pairingRequests\":" + std::to_string(control.pairing_requests);
        // Sender summary from the first active session.
        std::wstring sender_name{L"—"};
        std::wstring sender_address{L"—"};
        std::wstring session_state{L"—"};
        double volume_db = 0.0;
        bool has_session = false;
        for (const auto& session : snapshot.sessions) {
            if (session.owns_playback) {
                sender_name = session.sender_identity.empty()
                                  ? L"(未知设备)"
                                  : Utf8ToWide(session.sender_identity);
                sender_address = Utf8ToWide(session.peer_address);
                volume_db = session.volume_db;
                has_session = true;
                session_state = [&]() -> std::wstring {
                    switch (session.state) {
                    case session::SessionState::Connected:
                        return L"已连接";
                    case session::SessionState::Announced:
                        return L"已协商";
                    case session::SessionState::Ready:
                        return L"就绪";
                    case session::SessionState::Streaming:
                        return L"播放中";
                    case session::SessionState::Paused:
                        return L"已暂停";
                    case session::SessionState::Closing:
                        return L"结束中";
                    case session::SessionState::Closed:
                        return L"已关闭";
                    case session::SessionState::Error:
                        return L"错误";
                    }
                    return L"—";
                }();
                break;
            }
        }
        json += ",\"hasSession\":" + std::string(has_session ? "true" : "false");
        json += ",\"senderName\":";
        AppendJsonString(json, sender_name);
        json += ",\"senderAddress\":";
        AppendJsonString(json, sender_address);
        json += ",\"sessionState\":";
        AppendJsonString(json, session_state);
        json += ",\"volumeDb\":" + std::to_string(volume_db);
        // Transport.
        json += ",\"recording\":" + std::string(transport.recording ? "true" : "false");
        json += ",\"packets\":" + std::to_string(transport.datagrams_received);
        json += ",\"retransmitted\":" + std::to_string(transport.retransmitted_packets);
        json += ",\"resendRequests\":" + std::to_string(transport.resend_requests_sent);
        json += ",\"lost\":" + std::to_string(transport.jitter_buffer.lost_packets);
        json += ",\"late\":" + std::to_string(transport.jitter_buffer.late_packets);
        json += ",\"duplicate\":" + std::to_string(transport.jitter_buffer.duplicate_packets);
        json += ",\"reordered\":" + std::to_string(transport.jitter_buffer.reordered_packets);
        json += ",\"decoded\":" + std::to_string(transport.decoded_packets);
        json += ",\"concealed\":" + std::to_string(transport.concealed_packets);
        json += ",\"decoderErrors\":" + std::to_string(transport.decoder_errors);
        json += ",\"jitterUs\":" +
                std::to_string(transport.jitter_buffer.interarrival_jitter_microseconds);
        json += ",\"jitterP95Us\":" +
                std::to_string(transport.jitter_buffer.jitter_p95_microseconds);
        json += ",\"targetPackets\":" +
                std::to_string(transport.jitter_buffer.target_packets);
        json += ",\"adaptiveEnabled\":" +
                std::string(transport.jitter_buffer.adaptive_enabled ? "true" : "false");
        json += ",\"protocolLatencyUs\":" +
                std::to_string(transport.protocol_latency_microseconds);
        json += ",\"receiverLatencyUs\":" +
                std::to_string(transport.receiver_added_latency_estimate_microseconds);
        json += ",\"servoState\":";
        AppendJsonString(json, ServoStateName(transport.timing.servo.state));
        json += ",\"driftPpm\":" + std::to_string(transport.timing.servo.drift_ppm);
        json += ",\"rateCorrection\":" +
                std::to_string(transport.timing.servo.rate_correction);
        // Audio output.
        json += ",\"underruns\":" + std::to_string(output.underrun_count);
        json += ",\"renderedFrames\":" + std::to_string(output.rendered_frames);
        json += ",\"queueFrames\":" + std::to_string(output.current_buffer_depth_frames);
        json += ",\"latencyUs\":" + std::to_string(output.output_latency_microseconds);
        json += ",\"endpointFormat\":" +
                std::string(output.endpoint_sample_format ==
                                   audio::AudioEndpointSampleFormat::Pcm16
                               ? "\"PCM 16-bit\""
                               : "\"Float 32-bit\"");
        json += ",\"periodFrames\":" + std::to_string(output.engine_period_frames);
        json += ",\"outputMode\":";
        AppendJsonString(json, output.active_output_mode == audio::AudioOutputMode::Exclusive
                                   ? L"独占模式"
                                   : L"共享模式");
        json += ",\"lowLatencyActive\":" +
                std::string(output.low_latency_active ? "true" : "false");
        json += ",\"outputFallback\":" +
                std::string(output.output_mode_fallback ? "true" : "false");
        json += ",\"sessionVolumeActive\":" +
                std::string(output.session_volume_active ? "true" : "false");
        json += ",\"sessionVolumeScalar\":" +
                std::to_string(output.session_volume_scalar);
        json += ",\"transitionState\":";
        AppendJsonString(json, TransitionStateName(output.transition_state));
        json += ",\"clickPopEvents\":" + std::to_string(output.click_pop_events);
        json += ",\"clickPopMaxStep\":" + std::to_string(output.click_pop_maximum_step);
        json += ",\"invalidNumeric\":" + std::to_string(output.invalid_numeric_samples);
        json += ",\"clipped\":" + std::to_string(output.clipped_samples);
        json += ",\"dcEvents\":" + std::to_string(output.dc_offset_events);
        json += ",\"deviceSwitches\":" + std::to_string(output.device_switch_events);
        json += ",\"deviceRecoveries\":" +
                std::to_string(output.device_recovery_successes);
        json += ",\"deviceRecoveryFailures\":" +
                std::to_string(output.device_recovery_failures);
        json += ",\"hardResyncs\":" + std::to_string(output.hard_resync_events);
        json += ",\"underrunTransitions\":" +
                std::to_string(output.underrun_transition_events);
        json += ",\"safeMutes\":" + std::to_string(output.safe_mute_events);
        // Discovery.
        json += ",\"discoveryInterfaces\":" +
                std::to_string(discovery.active_interface_count);
        json += ",\"registrations\":" +
                std::to_string(discovery.registered_service_count);
        json += ",\"nameConflicts\":" + std::to_string(discovery.name_conflict_count);
        json += ",\"networkChanges\":" + std::to_string(discovery.network_change_count);
        json += ",\"registrationFailures\":" +
                std::to_string(discovery.registration_failure_count);
        // Lifecycle.
        json += ",\"suspendEvents\":" + std::to_string(snapshot.recovery.suspend_events);
        json += ",\"resumeEvents\":" + std::to_string(snapshot.recovery.resume_events);
        json += ",\"recoveryAttempts\":" +
                std::to_string(snapshot.recovery.recovery_attempts);
        json += ",\"recoveryFailures\":" +
                std::to_string(snapshot.recovery.recovery_failures);
        // TCP servers.
        json += ",\"raopActiveConnections\":" +
                std::to_string(snapshot.raop_server.active_connections);
        json += ",\"raopAccepted\":" +
                std::to_string(snapshot.raop_server.accepted_connections);
        json += ",\"raopTimeouts\":" + std::to_string(snapshot.raop_server.idle_timeouts);
        json += ",\"raopErrors\":" + std::to_string(snapshot.raop_server.transport_errors);
        json += "}";
    }

    [[nodiscard]] std::wstring ServoStateName(
        const timing::ClockServoState state) const noexcept {
        using timing::ClockServoState;
        switch (state) {
        case ClockServoState::Disabled:
            return L"未启用";
        case ClockServoState::Unlocked:
            return L"未锁定";
        case ClockServoState::Acquiring:
            return L"采集中";
        case ClockServoState::Locked:
            return L"已锁定";
        case ClockServoState::Holdover:
            return L"保持中";
        case ClockServoState::Relocking:
            return L"重锁定";
        }
        return L"—";
    }

    [[nodiscard]] std::wstring TransitionStateName(
        const audio::AudioTransitionState state) const noexcept {
        using audio::AudioTransitionState;
        switch (state) {
        case AudioTransitionState::Stopped:
            return L"已停止";
        case AudioTransitionState::Prewarming:
            return L"预热中";
        case AudioTransitionState::FadingIn:
            return L"淡入中";
        case AudioTransitionState::Audible:
            return L"播放中";
        case AudioTransitionState::FadingOut:
            return L"淡出中";
        case AudioTransitionState::Paused:
            return L"已暂停";
        case AudioTransitionState::SafeMute:
            return L"安全静音";
        case AudioTransitionState::DeviceMuted:
            return L"设备静音";
        }
        return L"—";
    }

    [[nodiscard]] std::string DevicesJson() noexcept {
        std::string json{"["};
        bool first = true;
        for (const auto& device : EnumerateAudioDevices()) {
            if (!first) {
                json += ",";
            }
            first = false;
            json += "{\"id\":";
            AppendJsonString(json, device.id);
            json += ",\"name\":";
            AppendJsonString(json, device.friendly_name);
            json += ",\"isDefault\":" + std::string(device.is_default ? "true" : "false");
            json += ",\"state\":" + std::to_string(device.state);
            json += "}";
        }
        json += "]";
        return json;
    }

    [[nodiscard]] std::string InterfacesJson() noexcept {
        std::string json{"["};
        bool first = true;
        for (const auto& info : EnumerateNetworkInterfaces(
                 settings_.include_virtual_interfaces)) {
            if (!first) {
                json += ",";
            }
            first = false;
            json += "{\"index\":" + std::to_string(info.ipv4_interface_index);
            json += ",\"ipv4\":";
            AppendJsonString(json, info.ipv4_address_text);
            json += ",\"name\":";
            AppendJsonString(json, info.friendly_name);
            json += ",\"description\":";
            AppendJsonString(json, info.description);
            json += ",\"virtual\":" + std::string(info.is_virtual ? "true" : "false");
            json += "}";
        }
        json += "]";
        return json;
    }

    // ---- Configuration application ----

    void ApplyConfigPayload(const std::string& payload) {
        bool restart_required = false;
        const auto apply_string = [&](const wchar_t* key, std::wstring& field,
                                      const bool live) {
            const auto value = ReadStringField(payload, key);
            if (payload.find("\"" + narrow(key) + "\"") == std::string::npos) {
                return;
            }
            if (field != value) {
                field = value;
                if (!live) {
                    restart_required = true;
                }
            }
        };
        const auto apply_bool = [&](const wchar_t* key, bool& field, const bool live) {
            const auto key_text = narrow(key);
            if (payload.find("\"" + key_text + "\"") == std::string::npos) {
                return;
            }
            const auto value = ReadBoolField(payload, key);
            if (field != value) {
                field = value;
                if (!live) {
                    restart_required = true;
                }
            }
        };
        const auto apply_u32 = [&](const wchar_t* key, std::uint32_t& field) {
            const auto key_text = narrow(key);
            if (payload.find("\"" + key_text + "\"") == std::string::npos) {
                return;
            }
            const auto value = static_cast<std::uint32_t>(ReadIntField(payload, key));
            if (field != value) {
                field = value;
                restart_required = true;
            }
        };
        apply_string(L"deviceName", settings_.device_name, true);
        apply_string(L"outputDeviceId", settings_.output_device_id, false);
        apply_bool(L"followDefaultDevice", settings_.follow_default_device, false);
        apply_bool(L"lowLatency", settings_.low_latency, false);
        apply_bool(L"exclusiveAudio", settings_.exclusive_audio, false);
        apply_bool(L"strictExclusive", settings_.strict_exclusive, false);
        {
            const auto key_text = narrow(L"raopPort");
            if (payload.find("\"" + key_text + "\"") != std::string::npos) {
                const auto value = static_cast<std::int64_t>(ReadIntField(payload, L"raopPort"));
                if (value > 0 && value <= 65'535 &&
                    settings_.raop_port != static_cast<std::uint16_t>(value)) {
                    settings_.raop_port = static_cast<std::uint16_t>(value);
                    restart_required = true;
                }
            }
        }
        {
            const auto key_text = narrow(L"airplayPort");
            if (payload.find("\"" + key_text + "\"") != std::string::npos) {
                const auto value =
                    static_cast<std::int64_t>(ReadIntField(payload, L"airplayPort"));
                if (value > 0 && value <= 65'535 &&
                    settings_.airplay_port != static_cast<std::uint16_t>(value)) {
                    settings_.airplay_port = static_cast<std::uint16_t>(value);
                    restart_required = true;
                }
            }
        }
        apply_bool(L"classicRaop", settings_.classic_raop, false);
        apply_bool(L"includeVirtualInterfaces", settings_.include_virtual_interfaces, false);
        apply_bool(L"experimentalBufferedTiming", settings_.experimental_buffered_timing,
                   false);
        apply_u32(L"bufferedTimingMilliseconds", settings_.buffered_timing_milliseconds);
        apply_bool(L"experimentalPtpTiming", settings_.experimental_ptp_timing, false);
        {
            const auto key_text = narrow(L"ptpInterfaceAddress");
            if (payload.find("\"" + key_text + "\"") != std::string::npos) {
                const auto value = ReadStringField(payload, L"ptpInterfaceAddress");
                if (narrow(value) != settings_.ptp_interface_address) {
                    settings_.ptp_interface_address = narrow(value);
                    restart_required = true;
                }
            }
        }
        apply_bool(L"minimizeToTray", settings_.minimize_to_tray, true);
        apply_u32(L"diagnosticsIntervalSeconds", settings_.diagnostics_interval_seconds);
        {
            const auto key_text = narrow(L"endpointCalibrationOffsetMicroseconds");
            if (payload.find("\"" + key_text + "\"") != std::string::npos) {
                const auto value = ReadIntField(payload, L"endpointCalibrationOffsetMicroseconds");
                if (settings_.endpoint_calibration_offset_microseconds != value) {
                    settings_.endpoint_calibration_offset_microseconds = value;
                    restart_required = true;
                }
            }
        }
        // The device name is live: republish DNS-SD on the running host.
        if (!receiver_->Running() ||
            receiver_->SetDeviceName(settings_.device_name)) {
            // Ok.
        }
        static_cast<void>(settings_store_.Save(settings_));
        PushStateToWebView();
    }

    // ---- Test tone ----

    void StartTestTone(const std::uint64_t id, const std::string& payload) {
        if (tone_thread_active_) {
            SendOkResponse(id, false, L"上一个测试音仍在播放。");
            return;
        }
        tone_thread_active_ = true;
        const auto signal_text = ReadStringField(payload, L"signal");
        const auto seconds = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(ReadIntField(payload, L"seconds"), 1, 60));
        airplaywin::audio::TestSignal signal{airplaywin::audio::TestSignal::Sine440Hz};
        if (signal_text == L"440") {
            signal = airplaywin::audio::TestSignal::Sine440Hz;
        } else if (signal_text == L"1000") {
            signal = airplaywin::audio::TestSignal::Sine1kHz;
        } else if (signal_text == L"sweep") {
            signal = airplaywin::audio::TestSignal::Sweep;
        } else if (signal_text == L"impulse") {
            signal = airplaywin::audio::TestSignal::Impulse;
        } else {
            signal = airplaywin::audio::TestSignal::Silence;
        }
        TestToneRequest request{
            .signal = signal,
            .output_device_id = settings_.output_device_id,
            .follow_default_device = settings_.follow_default_device,
            .low_latency = settings_.low_latency,
            .exclusive_audio = settings_.exclusive_audio,
            .strict_exclusive = settings_.strict_exclusive,
            .duration_seconds = seconds,
        };
        std::thread{[this, id, request]() {
            const auto result = PlayTestTone(request);
            std::string json{"{\"type\":\"response\",\"id\":"};
            json += std::to_string(id);
            json += ",\"ok\":";
            json += result.succeeded ? "true" : "false";
            json += ",\"result\":{\"renderedFrames\":" +
                    std::to_string(result.rendered_frames);
            json += ",\"underruns\":" + std::to_string(result.underrun_count);
            json += ",\"detail\":";
            AppendJsonString(json, result.detail);
            json += "}}";
            auto* payload = new std::string{std::move(json)};
            PostMessageW(hwnd_, kMessageToneFinished, 0U,
                         reinterpret_cast<LPARAM>(payload));
        }}.detach();
    }

    // ---- Firewall elevation ----

    void ElevateForFirewall(const wchar_t* arguments) noexcept {
        const auto path = ExecutablePath();
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            nullptr, L"runas", path.c_str(), arguments, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SendOkResponse(0U, false,
                           L"需要管理员权限：请以管理员身份运行本程序后重试，或从"
                           L"开始菜单右键选择“以管理员身份运行”。");
        }
    }

    // ---- Tray ----

    void CreateTrayIcon() noexcept {
        tray_data_.cbSize = sizeof(tray_data_);
        tray_data_.hWnd = hwnd_;
        tray_data_.uID = kTrayIconId;
        tray_data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        tray_data_.uCallbackMessage = kMessageTrayIcon;
        tray_data_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        wcscpy_s(tray_data_.szTip, L"AirPlayWin - Windows AirPlay 音频接收器");
        Shell_NotifyIconW(NIM_ADD, &tray_data_);
        tray_present_ = true;
    }

    void RemoveTrayIcon() noexcept {
        if (tray_present_) {
            Shell_NotifyIconW(NIM_DELETE, &tray_data_);
            tray_present_ = false;
        }
    }

    void ShowTrayMenu() noexcept {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kTrayCommandShow, L"打开 AirPlayWin");
        AppendMenuW(menu, MF_STRING, kTrayCommandToggle,
                    receiver_->Running() ? L"停止接收" : L"开始接收");
        AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayCommandExit, L"退出");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(hwnd_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x, point.y, 0U, hwnd_,
                       nullptr);
        DestroyMenu(menu);
    }

    void HideToTray() noexcept {
        ShowWindow(hwnd_, SW_HIDE);
        if (!tray_notice_shown_) {
            tray_notice_shown_ = true;
            tray_data_.uFlags = NIF_INFO;
            wcscpy_s(tray_data_.szInfoTitle, L"AirPlayWin 仍在运行");
            wcscpy_s(tray_data_.szInfo,
                     L"接收器继续在后台工作。双击托盘图标可重新打开窗口。");
            static_cast<void>(Shell_NotifyIconW(NIM_MODIFY, &tray_data_));
            tray_data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        }
    }

    HINSTANCE instance_{nullptr};
    HWND hwnd_{nullptr};
    AppSettings settings_{};
    std::filesystem::path ui_directory_{};
    std::filesystem::path log_directory_{};
    SettingsStore settings_store_;
    std::unique_ptr<ReceiverHost> receiver_;
    ComPtr<ICoreWebView2Environment> webview_environment_{};
    ComPtr<ICoreWebView2Controller> webview_controller_{};
    ComPtr<ICoreWebView2> webview_{};
    NOTIFYICONDATAW tray_data_{};
    bool tray_present_{false};
    bool tray_notice_shown_{false};
    bool test_bridge_{false};
    std::mutex lifecycle_work_mutex_{};
    std::atomic<bool> tone_thread_active_{false};
};

AppWindow::AppWindow(const HINSTANCE instance, AppSettings settings,
                     const std::filesystem::path ui_directory,
                     const std::filesystem::path log_directory,
                     const bool test_bridge)
    : impl_(std::make_unique<Impl>(instance, std::move(settings), ui_directory,
                                   log_directory, test_bridge)) {}

AppWindow::~AppWindow() = default;

bool AppWindow::Initialize(const int show_command) noexcept {
    return impl_ != nullptr && impl_->Initialize(show_command);
}

int AppWindow::Run() noexcept {
    return impl_ != nullptr ? impl_->Run() : 1;
}

void AppWindow::ShowWindowFromTray() noexcept {
    if (impl_ != nullptr) {
        impl_->ShowWindowFromTray();
    }
}

void AppWindow::RequestExit() noexcept {
    if (impl_ != nullptr) {
        impl_->RequestExit();
    }
}

}  // namespace airplaywin::ui
