#include <Windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <string>

#include "AirPlayWinVersion.h"
#include "app/PlatformCommands.h"
#include "app/SettingsStore.h"
#include "platform/windows/system/SingleInstanceGuard.h"
#include "ui/AppWindow.h"

namespace {

using airplaywin::app::AppSettings;
using airplaywin::app::RunFirewallCommand;
using airplaywin::app::SettingsStore;
using airplaywin::ui::AppWindow;
using airplaywin::windows::system::SingleInstanceGuard;

[[nodiscard]] std::filesystem::path LocalAppDataDirectory() {
    wchar_t* path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0U, nullptr, &path)) ||
        path == nullptr) {
        return {};
    }
    std::filesystem::path result{path};
    CoTaskMemFree(path);
    return result / L"AirPlayWin";
}

[[nodiscard]] bool IsElevated() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0U;
    const auto result =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return result && elevation.TokenIsElevated != 0U;
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

void RelaunchElevated(const std::wstring& arguments) {
    const auto path = ExecutablePath();
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"runas", path.c_str(), arguments.c_str(), nullptr, SW_SHOWNORMAL));
    std::wstring message;
    if (result > 32) {
        message = L"已请求管理员权限。请在弹出的确认对话框中允许后继续。";
    } else {
        message = L"需要管理员权限来修改防火墙规则。\n请右键点击 AirPlayWin 并选择"
                  L"“以管理员身份运行”，然后重试。";
    }
    MessageBoxW(nullptr, message.c_str(), L"AirPlayWin - 防火墙", MB_ICONINFORMATION);
}

[[nodiscard]] int RunFirewallManagement(const std::wstring& mode,
                                        const AppSettings& settings) {
    bool install = mode == L"--install-firewall-rules";
    bool remove = mode == L"--remove-firewall-rules";
    bool query = mode == L"--firewall-status";
    if (!install && !remove && !query) {
        return 1;
    }
    const auto result = RunFirewallCommand(
        ExecutablePath(), settings.raop_port, settings.airplay_port,
        settings.classic_raop, install, remove, query);
    std::wstring title;
    if (install) {
        title = L"防火墙规则安装";
    } else if (remove) {
        title = L"防火墙规则移除";
    } else {
        title = L"防火墙规则状态";
    }
    std::wstring message =
        result.succeeded ? L"操作成功。\n\n" : L"操作未能完全成功。\n\n";
    message += result.detail;
    MessageBoxW(nullptr, message.c_str(), (L"AirPlayWin - " + title).c_str(),
                result.succeeded ? MB_ICONINFORMATION : MB_ICONWARNING);
    return result.succeeded ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    // Per-monitor DPI awareness keeps text and UI crisp on scaled displays
    // (125% / 150% / 200%): without it Windows bitmap-stretches the window,
    // which is what makes fonts look blurry. The manifest declares the same
    // setting for the process start; the API covers hosts that ignore it.
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ==
        FALSE) {
        static_cast<void>(SetProcessDPIAware());
    }

    // WebView2 requires a COM-initialized STA on the UI thread.
    static_cast<void>(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    // Management modes: firewall rules need elevation. When the request
    // arrives unelevated, relaunch and let the elevated copy do the work.
    AppSettings settings;
    const auto app_directory = LocalAppDataDirectory();
    SettingsStore store{app_directory / "settings.json"};
    static_cast<void>(store.Load(settings));

    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    bool management_mode = false;
    bool minimized = false;
    bool test_bridge = false;
    std::filesystem::path ui_path;
    if (arguments != nullptr) {
        for (int index = 1; index < argument_count; ++index) {
            const std::wstring argument{arguments[index]};
            if (argument == L"--minimized" || argument == L"--background") {
                minimized = true;
            } else if (argument == L"--test-bridge") {
                // The window stays visible: hidden WebView2 pages get
                // Chromium background throttling, which would stall the
                // test sequence's timers.
                test_bridge = true;
            } else if (argument == L"--ui-path" && index + 1 < argument_count) {
                ui_path = arguments[++index];
            } else if (argument == L"--install-firewall-rules" ||
                       argument == L"--remove-firewall-rules" ||
                       argument == L"--firewall-status") {
                management_mode = true;
                if (!IsElevated()) {
                    RelaunchElevated(argument);
                    return 0;
                }
                const auto result = RunFirewallManagement(argument, settings);
                LocalFree(arguments);
                return result;
            }
        }
        LocalFree(arguments);
    }
    if (management_mode) {
        return 0;
    }

    // Single instance: a second launch simply focuses the existing window.
    SingleInstanceGuard guard{L"Local\\AirPlayWin.App"};
    if (!guard.Acquired()) {
        if (guard.AlreadyRunning()) {
            const auto existing = FindWindowW(L"AirPlayWinAppWindow", nullptr);
            if (existing != nullptr) {
                ShowWindow(existing, SW_SHOW);
                SetForegroundWindow(existing);
            } else {
                MessageBoxW(nullptr,
                            L"AirPlayWin 已经在运行。请查看系统托盘中的图标。",
                            L"AirPlayWin", MB_ICONINFORMATION);
            }
            return 0;
        }
        MessageBoxW(nullptr, L"无法创建单实例保护。", L"AirPlayWin", MB_ICONERROR);
        return 1;
    }

    if (ui_path.empty()) {
        wchar_t executable[32'767U]{};
        const auto length = GetModuleFileNameW(nullptr, executable,
                                               static_cast<DWORD>(std::size(executable)));
        std::filesystem::path base = length > 0U
                                         ? std::filesystem::path{executable}.parent_path()
                                         : std::filesystem::current_path();
        ui_path = base / L"ui";
    }

    AppWindow window{instance, settings, ui_path, app_directory / L"Logs", test_bridge};
    if (!window.Initialize(minimized ? SW_HIDE : show_command)) {
        MessageBoxW(nullptr,
                    L"窗口初始化失败。请查看 %LOCALAPPDATA%\\AirPlayWin\\Logs 中的日志。",
                    L"AirPlayWin", MB_ICONERROR);
        return 1;
    }
    return window.Run();
}
