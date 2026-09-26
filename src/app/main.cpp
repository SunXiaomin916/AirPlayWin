#include <Windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/audio/LoopbackLatencyAnalyzer.h"
#include "core/audio/TestSignalGenerator.h"
#include "core/crypto/OpenSessionAuthenticator.h"
#include "core/discovery/AirPlayServiceRecords.h"
#include "core/group/GroupSyncAnalyzer.h"
#include "core/lifecycle/RecoveryCoordinator.h"
#include "core/protocol/AirPlayControlService.h"
#include "core/timing/PtpClockDomain.h"
#include "platform/windows/audio/WindowsAudioDeviceEnumerator.h"
#include "platform/windows/audio/WindowsAudioEngine.h"
#include "platform/windows/audio/WindowsAudioStreamSink.h"
#include "platform/windows/audio/WindowsEndpointCalibrationStore.h"
#include "platform/windows/crypto/WindowsRaopCryptoProvider.h"
#include "platform/windows/network/WindowsDiscoveryService.h"
#include "platform/windows/network/IocpTcpServer.h"
#include "platform/windows/network/WindowsNetworkInterfaceEnumerator.h"
#include "platform/windows/network/WindowsRtpTransportController.h"
#include "platform/windows/system/SingleInstanceGuard.h"
#include "platform/windows/system/WindowsFirewallManager.h"
#include "platform/windows/system/WindowsPowerEventMonitor.h"
#include "platform/windows/timing/QpcClock.h"
#include "platform/windows/timing/WindowsPtpTimingService.h"
#include "app/WaveFileReader.h"

namespace {

using airplaywin::audio::AudioFormat;
using airplaywin::audio::AudioClientPath;
using airplaywin::audio::AudioOutputMode;
using airplaywin::audio::AudioTransitionState;
using airplaywin::audio::TestSignal;
using airplaywin::audio::TestSignalGenerator;
using airplaywin::windows::audio::WasapiOutputOptions;
using airplaywin::windows::audio::WindowsAudioDeviceEnumerator;
using airplaywin::windows::audio::WindowsAudioEngine;
using airplaywin::windows::audio::WindowsAudioStreamSink;
using airplaywin::windows::audio::WindowsEndpointCalibrationStore;
using airplaywin::windows::crypto::WindowsRaopCryptoProvider;
using airplaywin::windows::network::WindowsDiscoveryService;
using airplaywin::windows::network::IocpTcpServer;
using airplaywin::windows::network::WindowsNetworkInterfaceEnumerator;
using airplaywin::windows::network::WindowsRtpTransportController;
using airplaywin::windows::system::FirewallRuleHealth;
using airplaywin::windows::system::PowerLifecycleEvent;
using airplaywin::windows::system::SingleInstanceGuard;
using airplaywin::windows::system::WindowsFirewallManager;
using airplaywin::windows::system::WindowsPowerEventMonitor;
using airplaywin::windows::timing::QpcClock;
using airplaywin::windows::timing::WindowsPtpTimingService;

[[nodiscard]] std::wstring_view TransitionStateName(
    const AudioTransitionState state) noexcept {
    switch (state) {
    case AudioTransitionState::Stopped:
        return L"stopped";
    case AudioTransitionState::Prewarming:
        return L"prewarming";
    case AudioTransitionState::FadingIn:
        return L"fading-in";
    case AudioTransitionState::Audible:
        return L"audible";
    case AudioTransitionState::FadingOut:
        return L"fading-out";
    case AudioTransitionState::Paused:
        return L"paused";
    case AudioTransitionState::SafeMute:
        return L"safe-mute";
    case AudioTransitionState::DeviceMuted:
        return L"device-muted";
    }
    return L"unknown";
}

[[nodiscard]] std::wstring_view OutputModeName(const AudioOutputMode mode) noexcept {
    return mode == AudioOutputMode::Exclusive ? L"exclusive" : L"shared";
}

[[nodiscard]] std::wstring_view AudioClientPathName(const AudioClientPath path) noexcept {
    return path == AudioClientPath::AudioClient3 ? L"IAudioClient3" : L"legacy";
}

[[nodiscard]] std::wstring_view EndpointSampleFormatName(
    const airplaywin::audio::AudioEndpointSampleFormat format) noexcept {
    return format == airplaywin::audio::AudioEndpointSampleFormat::Pcm16 ? L"pcm16"
                                                                         : L"float32";
}

[[nodiscard]] std::wstring_view AdaptiveJitterStateName(
    const airplaywin::transport::AdaptiveJitterState state) noexcept {
    using airplaywin::transport::AdaptiveJitterState;
    switch (state) {
    case AdaptiveJitterState::Warmup:
        return L"warmup";
    case AdaptiveJitterState::Locked:
        return L"locked";
    case AdaptiveJitterState::LowLatency:
        return L"low-latency";
    case AdaptiveJitterState::Degraded:
        return L"degraded";
    case AdaptiveJitterState::Recovery:
        return L"recovery";
    }
    return L"unknown";
}

[[nodiscard]] std::wstring_view ClockServoStateName(
    const airplaywin::timing::ClockServoState state) noexcept {
    using airplaywin::timing::ClockServoState;
    switch (state) {
    case ClockServoState::Disabled:
        return L"disabled";
    case ClockServoState::Unlocked:
        return L"unlocked";
    case ClockServoState::Acquiring:
        return L"acquiring";
    case ClockServoState::Locked:
        return L"locked";
    case ClockServoState::Holdover:
        return L"holdover";
    case ClockServoState::Relocking:
        return L"relocking";
    }
    return L"unknown";
}

[[nodiscard]] std::wstring_view LoopbackStatusName(
    const airplaywin::audio::LoopbackLatencyStatus status) noexcept {
    using airplaywin::audio::LoopbackLatencyStatus;
    switch (status) {
    case LoopbackLatencyStatus::Ok:
        return L"ok";
    case LoopbackLatencyStatus::InvalidConfiguration:
        return L"invalid-configuration";
    case LoopbackLatencyStatus::ReferenceOnsetNotFound:
        return L"reference-onset-not-found";
    case LoopbackLatencyStatus::OutputOnsetNotFound:
        return L"output-onset-not-found";
    case LoopbackLatencyStatus::LatencyOutOfRange:
        return L"latency-out-of-range";
    }
    return L"unknown";
}

[[nodiscard]] std::wstring_view GroupSyncStatusName(
    const airplaywin::group::GroupSyncAnalysisStatus status) noexcept {
    using airplaywin::group::GroupSyncAnalysisStatus;
    switch (status) {
    case GroupSyncAnalysisStatus::Ok:
        return L"ok";
    case GroupSyncAnalysisStatus::InvalidConfiguration:
        return L"invalid-configuration";
    case GroupSyncAnalysisStatus::NoReferencePulses:
        return L"no-reference-pulses";
    case GroupSyncAnalysisStatus::InsufficientCompletePulses:
        return L"insufficient-complete-pulses";
    }
    return L"unknown";
}

[[nodiscard]] std::atomic_bool& ShutdownRequestedFlag() noexcept {
    static std::atomic_bool requested{false};
    return requested;
}

BOOL WINAPI HandleConsoleControl(const DWORD type) noexcept {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        ShutdownRequestedFlag().store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

class ScopedConsoleControlHandler final {
public:
    ScopedConsoleControlHandler() noexcept {
        ShutdownRequestedFlag().store(false, std::memory_order_release);
        installed_ = SetConsoleCtrlHandler(&HandleConsoleControl, TRUE) != FALSE;
    }

    ~ScopedConsoleControlHandler() {
        if (installed_) {
            static_cast<void>(SetConsoleCtrlHandler(&HandleConsoleControl, FALSE));
        }
    }

    ScopedConsoleControlHandler(const ScopedConsoleControlHandler&) = delete;
    ScopedConsoleControlHandler& operator=(const ScopedConsoleControlHandler&) = delete;

private:
    bool installed_{false};
};

struct CommandLine final {
    bool show_help{false};
    bool list_devices{false};
    bool list_network_interfaces{false};
    bool play{false};
    bool discover{false};
    bool serve{false};
    bool firewall_status{false};
    bool install_firewall_rules{false};
    bool remove_firewall_rules{false};
    bool lifecycle_smoke{false};
    bool run_until_stopped{false};
    bool experimental_buffered_timing{false};
    bool experimental_ptp_timing{false};
    bool low_latency{false};
    bool exclusive_audio{false};
    bool strict_exclusive{false};
    bool analyze_loopback{false};
    bool analyze_group_sync{false};
    bool save_endpoint_calibration{false};
    bool show_endpoint_calibration{false};
    bool clear_endpoint_calibration{false};
    std::uint32_t buffered_timing_milliseconds{120U};
    std::string ptp_interface_address{"0.0.0.0"};
    std::filesystem::path loopback_capture_path{};
    std::filesystem::path group_sync_capture_path{};
    std::uint32_t loopback_reference_channel{0U};
    std::uint32_t loopback_output_channel{1U};
    bool loopback_reference_channel_set{false};
    bool loopback_output_channel_set{false};
    bool loopback_analysis_option_set{false};
    bool group_analysis_option_set{false};
    bool onset_threshold_set{false};
    std::optional<std::uint64_t> loopback_stimulus_frame{};
    float loopback_onset_threshold{0.25F};
    std::uint32_t loopback_maximum_latency_milliseconds{1'000U};
    std::vector<std::uint16_t> group_sync_channels{};
    std::uint32_t group_sync_minimum_pulse_gap_milliseconds{100U};
    std::uint32_t group_sync_maximum_skew_milliseconds{100U};
    std::wstring device_id{};
    std::int64_t endpoint_calibration_offset_microseconds{0};
    bool endpoint_calibration_offset_set{false};
    std::wstring discovery_name{L"AirPlayWin"};
    airplaywin::discovery::DeviceId discovery_device_id{};
    std::uint16_t raop_port{5'000U};
    std::uint16_t airplay_port{7'000U};
    bool classic_raop{false};
    bool include_virtual_interfaces{false};
    TestSignal signal{TestSignal::Sine440Hz};
    std::uint32_t sample_rate{48'000U};
    std::uint32_t duration_seconds{10U};
    std::uint32_t diagnostics_interval_seconds{1U};
    std::uint32_t transition_cycles{0U};
};

void PrintUsage() {
    std::wcout
        << L"AirPlayWin phase-12 synchronization regression tools\n\n"
        << L"  AirPlayWin --help\n"
        << L"  AirPlayWin --list-devices\n"
        << L"  AirPlayWin --list-network-interfaces [--include-virtual-interfaces]\n"
        << L"  AirPlayWin --play [--device <endpoint-id>]\n"
        << L"             [--signal 440|1000|silence|impulse|latency-pulse|sweep]\n"
        << L"             [--sample-rate <8000..384000>] [--duration <seconds>]\n"
        << L"             [--transition-cycles <count>]\n"
        << L"             [--low-latency] [--exclusive [--strict-exclusive]]\n"
        << L"             [--endpoint-offset-us <-1000000..1000000>]\n"
        << L"             [--diagnostics-interval <seconds>]\n\n"
        << L"  AirPlayWin --discover [--name <speaker-name>] [--device-id <AA:BB:CC:DD:EE:FF>]\n"
        << L"             [--raop-port <port>] [--airplay-port <port>] [--duration <seconds>]\n"
        << L"             [--classic-raop]\n"
        << L"             [--include-virtual-interfaces]\n\n"
        << L"  AirPlayWin --serve [--name <speaker-name>] [--device-id <AA:BB:CC:DD:EE:FF>]\n"
        << L"             [--device <endpoint-id>]\n"
        << L"             [--low-latency] [--exclusive [--strict-exclusive]]\n"
        << L"             [--endpoint-offset-us <-1000000..1000000>]\n"
        << L"             [--raop-port <port>] [--airplay-port <port>] [--duration <seconds>]\n"
        << L"             [--run-until-stopped] [--diagnostics-interval <seconds>]\n"
        << L"             [--experimental-buffered-timing [--buffered-timing-ms <20..2000>]]\n"
        << L"             [--experimental-ptp-timing [--ptp-interface <IPv4>]]\n"
        << L"             [--classic-raop]\n"
        << L"             [--include-virtual-interfaces]\n\n"
        << L"  AirPlayWin --analyze-loopback <capture.wav>\n"
        << L"             [--reference-channel <index> --output-channel <index>]\n"
        << L"             [--stimulus-frame <frame> --output-channel <index>]\n"
        << L"             [--onset-threshold <0.01..1.0>] [--max-latency-ms <1..10000>]\n\n"
        << L"  AirPlayWin --analyze-group-sync <capture.wav>\n"
        << L"             [--group-channels <0,1[,2,3]>]\n"
        << L"             [--onset-threshold <0.01..1.0>] [--pulse-gap-ms <10..10000>]\n"
        << L"             [--max-skew-ms <1..1000>]\n\n"
        << L"  AirPlayWin --save-endpoint-offset --device <endpoint-id>\n"
        << L"             --endpoint-offset-us <-1000000..1000000>\n"
        << L"  AirPlayWin --show-endpoint-offset --device <endpoint-id>\n"
        << L"  AirPlayWin --clear-endpoint-offset --device <endpoint-id>\n\n"
        << L"  AirPlayWin --firewall-status [--raop-port <port>] [--airplay-port <port>]\n"
        << L"  AirPlayWin --install-firewall-rules [--raop-port <port>] [--airplay-port <port>]\n"
        << L"  AirPlayWin --remove-firewall-rules\n\n"
        << L"Examples:\n"
        << L"  AirPlayWin --play --signal 440 --duration 1800\n"
        << L"  AirPlayWin --play --device \"{endpoint-id}\" --transition-cycles 1000\n"
        << L"  AirPlayWin --play --low-latency --exclusive --signal 440\n"
        << L"  AirPlayWin --play --exclusive --signal latency-pulse --duration 5\n"
        << L"  AirPlayWin --analyze-loopback capture.wav --reference-channel 0 --output-channel 1\n"
        << L"  AirPlayWin --analyze-group-sync group.wav --group-channels 0,1,2,3\n"
        << L"  AirPlayWin --discover --name \"Living Room PC\" --duration 300\n"
        << L"  AirPlayWin --serve --name \"Living Room PC\" --duration 300\n";
}

[[nodiscard]] bool ParseSignedMicroseconds(const wchar_t* const text,
                                           std::int64_t& value) {
    errno = 0;
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoll(text, &end, 10);
    if (errno == ERANGE || end == text || *end != L'\0' ||
        parsed < -1'000'000LL || parsed > 1'000'000LL) {
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]] bool ParseUnsigned(const wchar_t* const text, std::uint32_t& value) {
    if (text == nullptr || *text == L'\0' || *text == L'-') {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != L'\0' ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

[[nodiscard]] bool ParseUnsigned64(const wchar_t* const text, std::uint64_t& value) {
    if (text == nullptr || *text == L'\0' || *text == L'-') {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != L'\0') {
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]] bool ParseChannelList(const wchar_t* const text,
                                    std::vector<std::uint16_t>& channels) {
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    channels.clear();
    const std::wstring value{text};
    std::size_t begin = 0U;
    while (begin < value.size()) {
        const auto end = value.find(L',', begin);
        const auto token = value.substr(begin, end == std::wstring::npos
                                                   ? std::wstring::npos
                                                   : end - begin);
        std::uint32_t channel = 0U;
        if (token.empty() || !ParseUnsigned(token.c_str(), channel) || channel > 7U ||
            std::ranges::find(channels, static_cast<std::uint16_t>(channel)) !=
                channels.end()) {
            return false;
        }
        channels.push_back(static_cast<std::uint16_t>(channel));
        if (end == std::wstring::npos) {
            break;
        }
        begin = end + 1U;
    }
    return channels.size() >= 2U && channels.size() <= 4U;
}

[[nodiscard]] bool ParseFloat(const wchar_t* const text, float& value) {
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const auto parsed = std::wcstof(text, &end);
    if (errno == ERANGE || end == text || *end != L'\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]] bool ParseSignal(const std::wstring& value, TestSignal& signal) {
    if (value == L"440") {
        signal = TestSignal::Sine440Hz;
    } else if (value == L"1000") {
        signal = TestSignal::Sine1kHz;
    } else if (value == L"silence") {
        signal = TestSignal::Silence;
    } else if (value == L"impulse") {
        signal = TestSignal::Impulse;
    } else if (value == L"latency-pulse") {
        signal = TestSignal::LatencyPulse;
    } else if (value == L"sweep") {
        signal = TestSignal::Sweep;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool ParsePort(const wchar_t* const text, std::uint16_t& port) {
    std::uint32_t parsed = 0U;
    if (!ParseUnsigned(text, parsed) || parsed == 0U || parsed > UINT16_MAX) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

[[nodiscard]] bool ParseCommandLine(const int argc, wchar_t* argv[], CommandLine& command) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument{argv[index]};
        if (argument == L"--help" || argument == L"-h") {
            command.show_help = true;
        } else if (argument == L"--list-devices") {
            command.list_devices = true;
        } else if (argument == L"--list-network-interfaces") {
            command.list_network_interfaces = true;
        } else if (argument == L"--play") {
            command.play = true;
        } else if (argument == L"--discover") {
            command.discover = true;
        } else if (argument == L"--serve") {
            command.serve = true;
        } else if (argument == L"--firewall-status") {
            command.firewall_status = true;
        } else if (argument == L"--install-firewall-rules") {
            command.install_firewall_rules = true;
        } else if (argument == L"--remove-firewall-rules") {
            command.remove_firewall_rules = true;
        } else if (argument == L"--lifecycle-smoke") {
            command.lifecycle_smoke = true;
        } else if (argument == L"--run-until-stopped") {
            command.run_until_stopped = true;
        } else if (argument == L"--experimental-buffered-timing") {
            command.experimental_buffered_timing = true;
        } else if (argument == L"--experimental-ptp-timing") {
            command.experimental_ptp_timing = true;
        } else if (argument == L"--ptp-interface" && index + 1 < argc) {
            const std::wstring value{argv[++index]};
            if (value.empty() || value.size() > 15U ||
                !std::all_of(value.begin(), value.end(), [](const wchar_t character) {
                    return (character >= L'0' && character <= L'9') || character == L'.';
                })) {
                return false;
            }
            command.ptp_interface_address.clear();
            command.ptp_interface_address.reserve(value.size());
            for (const auto character : value) {
                command.ptp_interface_address.push_back(static_cast<char>(character));
            }
            command.experimental_ptp_timing = true;
        } else if (argument == L"--low-latency") {
            command.low_latency = true;
        } else if (argument == L"--exclusive") {
            command.exclusive_audio = true;
            command.low_latency = true;
        } else if (argument == L"--strict-exclusive") {
            command.strict_exclusive = true;
            command.exclusive_audio = true;
            command.low_latency = true;
        } else if (argument == L"--analyze-loopback" && index + 1 < argc) {
            command.analyze_loopback = true;
            command.loopback_capture_path = argv[++index];
        } else if (argument == L"--analyze-group-sync" && index + 1 < argc) {
            command.analyze_group_sync = true;
            command.group_sync_capture_path = argv[++index];
        } else if (argument == L"--save-endpoint-offset") {
            command.save_endpoint_calibration = true;
        } else if (argument == L"--show-endpoint-offset") {
            command.show_endpoint_calibration = true;
        } else if (argument == L"--clear-endpoint-offset") {
            command.clear_endpoint_calibration = true;
        } else if (argument == L"--reference-channel" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], command.loopback_reference_channel) ||
                command.loopback_reference_channel > 7U) {
                return false;
            }
            command.loopback_reference_channel_set = true;
            command.loopback_analysis_option_set = true;
        } else if (argument == L"--output-channel" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], command.loopback_output_channel) ||
                command.loopback_output_channel > 7U) {
                return false;
            }
            command.loopback_output_channel_set = true;
            command.loopback_analysis_option_set = true;
        } else if (argument == L"--stimulus-frame" && index + 1 < argc) {
            std::uint64_t stimulus_frame = 0U;
            if (!ParseUnsigned64(argv[++index], stimulus_frame)) {
                return false;
            }
            command.loopback_stimulus_frame = stimulus_frame;
            command.loopback_analysis_option_set = true;
        } else if (argument == L"--onset-threshold" && index + 1 < argc) {
            if (!ParseFloat(argv[++index], command.loopback_onset_threshold) ||
                command.loopback_onset_threshold < 0.01F ||
                command.loopback_onset_threshold > 1.0F) {
                return false;
            }
            command.onset_threshold_set = true;
        } else if (argument == L"--max-latency-ms" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index],
                               command.loopback_maximum_latency_milliseconds) ||
                command.loopback_maximum_latency_milliseconds < 1U ||
                command.loopback_maximum_latency_milliseconds > 10'000U) {
                return false;
            }
            command.loopback_analysis_option_set = true;
        } else if (argument == L"--group-channels" && index + 1 < argc) {
            if (!ParseChannelList(argv[++index], command.group_sync_channels)) {
                return false;
            }
            command.group_analysis_option_set = true;
        } else if (argument == L"--pulse-gap-ms" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index],
                               command.group_sync_minimum_pulse_gap_milliseconds) ||
                command.group_sync_minimum_pulse_gap_milliseconds < 10U ||
                command.group_sync_minimum_pulse_gap_milliseconds > 10'000U) {
                return false;
            }
            command.group_analysis_option_set = true;
        } else if (argument == L"--max-skew-ms" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index],
                               command.group_sync_maximum_skew_milliseconds) ||
                command.group_sync_maximum_skew_milliseconds < 1U ||
                command.group_sync_maximum_skew_milliseconds > 1'000U) {
                return false;
            }
            command.group_analysis_option_set = true;
        } else if (argument == L"--endpoint-offset-us" && index + 1 < argc) {
            if (!ParseSignedMicroseconds(
                    argv[++index], command.endpoint_calibration_offset_microseconds)) {
                return false;
            }
            command.endpoint_calibration_offset_set = true;
        } else if (argument == L"--buffered-timing-ms" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], command.buffered_timing_milliseconds) ||
                command.buffered_timing_milliseconds < 20U ||
                command.buffered_timing_milliseconds > 2'000U) {
                return false;
            }
            command.experimental_buffered_timing = true;
        } else if (argument == L"--device" && index + 1 < argc) {
            command.device_id = argv[++index];
        } else if (argument == L"--name" && index + 1 < argc) {
            command.discovery_name = argv[++index];
        } else if (argument == L"--device-id" && index + 1 < argc) {
            if (!airplaywin::discovery::ParseDeviceId(argv[++index],
                                                       command.discovery_device_id)) {
                return false;
            }
        } else if (argument == L"--raop-port" && index + 1 < argc) {
            if (!ParsePort(argv[++index], command.raop_port)) {
                return false;
            }
        } else if (argument == L"--airplay-port" && index + 1 < argc) {
            if (!ParsePort(argv[++index], command.airplay_port)) {
                return false;
            }
        } else if (argument == L"--classic-raop") {
            command.classic_raop = true;
        } else if (argument == L"--include-virtual-interfaces") {
            command.include_virtual_interfaces = true;
        } else if (argument == L"--signal" && index + 1 < argc) {
            if (!ParseSignal(argv[++index], command.signal)) {
                return false;
            }
        } else if (argument == L"--sample-rate" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], command.sample_rate) ||
                command.sample_rate < 8'000U || command.sample_rate > 384'000U) {
                return false;
            }
        } else if (argument == L"--duration" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], command.duration_seconds) ||
                command.duration_seconds == 0U) {
                return false;
            }
        } else if (argument == L"--transition-cycles" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], command.transition_cycles)) {
                return false;
            }
        } else if (argument == L"--diagnostics-interval" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], command.diagnostics_interval_seconds) ||
                command.diagnostics_interval_seconds == 0U) {
                return false;
            }
        } else {
            return false;
        }
    }
    const auto firewall_commands = static_cast<unsigned>(command.firewall_status) +
                                   static_cast<unsigned>(command.install_firewall_rules) +
                                   static_cast<unsigned>(command.remove_firewall_rules);
    const auto calibration_commands =
        static_cast<unsigned>(command.save_endpoint_calibration) +
        static_cast<unsigned>(command.show_endpoint_calibration) +
        static_cast<unsigned>(command.clear_endpoint_calibration);
    const auto analysis_commands = static_cast<unsigned>(command.analyze_loopback) +
                                   static_cast<unsigned>(command.analyze_group_sync);
    if (firewall_commands > 1U || (command.lifecycle_smoke && !command.serve) ||
        (command.run_until_stopped && !command.serve) ||
        (command.experimental_buffered_timing && !command.serve) ||
        (command.experimental_ptp_timing && !command.serve) ||
        (command.experimental_buffered_timing && command.experimental_ptp_timing) ||
        (command.low_latency && !command.play && !command.serve) ||
        (command.strict_exclusive && !command.exclusive_audio) ||
        calibration_commands > 1U || analysis_commands > 1U ||
        (calibration_commands != 0U && command.device_id.empty()) ||
        (command.save_endpoint_calibration && !command.endpoint_calibration_offset_set) ||
        ((command.show_endpoint_calibration || command.clear_endpoint_calibration) &&
         command.endpoint_calibration_offset_set) ||
        (command.endpoint_calibration_offset_set && !command.play && !command.serve &&
         !command.save_endpoint_calibration)) {
        return false;
    }
    if (firewall_commands != 0U &&
        (command.play || command.discover || command.serve || command.list_devices ||
         command.list_network_interfaces)) {
        return false;
    }
    if (calibration_commands != 0U &&
        (command.play || command.discover || command.serve || command.list_devices ||
         command.list_network_interfaces || analysis_commands != 0U ||
         firewall_commands != 0U)) {
        return false;
    }
    if (command.analyze_loopback &&
        (command.play || command.discover || command.serve || command.list_devices ||
         command.list_network_interfaces || firewall_commands != 0U ||
         command.loopback_capture_path.empty() ||
         (command.loopback_stimulus_frame.has_value() &&
          command.loopback_reference_channel_set))) {
        return false;
    }
    if (command.analyze_group_sync &&
        (command.play || command.discover || command.serve || command.list_devices ||
         command.list_network_interfaces || firewall_commands != 0U ||
         command.group_sync_capture_path.empty())) {
        return false;
    }
    if ((!command.analyze_loopback && command.loopback_analysis_option_set) ||
        (!command.analyze_group_sync && command.group_analysis_option_set) ||
        (analysis_commands == 0U && command.onset_threshold_set)) {
        return false;
    }
    if (command.analyze_loopback && command.loopback_stimulus_frame.has_value() &&
        !command.loopback_output_channel_set) {
        command.loopback_output_channel = 0U;
    }
    return true;
}

[[nodiscard]] int RunEndpointCalibrationCommand(const CommandLine& command) {
    WindowsEndpointCalibrationStore store;
    if (command.save_endpoint_calibration) {
        if (!store.SaveWindowsEndpoint(
                command.device_id,
                command.endpoint_calibration_offset_microseconds,
                airplaywin::group::EndpointCalibrationSource::Manual)) {
            std::wcerr << L"Unable to save endpoint calibration; error=0x" << std::hex
                       << store.LastError() << std::dec << L".\n";
            return 27;
        }
        std::wcout << L"Saved endpoint latency offset: device=\"" << command.device_id
                   << L"\", offset_us="
                   << command.endpoint_calibration_offset_microseconds << L".\n";
        return 0;
    }
    if (command.clear_endpoint_calibration) {
        if (!store.RemoveWindowsEndpoint(command.device_id)) {
            std::wcerr << L"Unable to clear endpoint calibration; error=0x" << std::hex
                       << store.LastError() << std::dec << L".\n";
            return 28;
        }
        std::wcout << L"Cleared endpoint latency offset for device=\""
                   << command.device_id << L"\".\n";
        return 0;
    }
    const auto calibration = store.LoadWindowsEndpoint(command.device_id);
    if (!calibration.has_value()) {
        std::wcerr << L"No endpoint calibration is available; error=0x" << std::hex
                   << store.LastError() << std::dec << L".\n";
        return 29;
    }
    std::wcout << L"Endpoint latency offset: device=\"" << command.device_id
               << L"\", offset_us=" << calibration->offset_microseconds
               << L", source="
               << (calibration->source ==
                           airplaywin::group::EndpointCalibrationSource::Measured
                       ? L"measured"
                       : L"manual")
               << L".\n";
    return 0;
}

void LoadPersistedEndpointCalibration(CommandLine& command) {
    if (command.device_id.empty() || command.endpoint_calibration_offset_set ||
        (!command.play && !command.serve)) {
        return;
    }
    WindowsEndpointCalibrationStore store;
    if (const auto calibration = store.LoadWindowsEndpoint(command.device_id)) {
        command.endpoint_calibration_offset_microseconds =
            calibration->offset_microseconds;
        std::wcout << L"Loaded endpoint latency offset: "
                   << calibration->offset_microseconds << L" us.\n";
    }
}

[[nodiscard]] std::wstring_view FirewallHealthText(
    const FirewallRuleHealth health) noexcept {
    switch (health) {
    case FirewallRuleHealth::Missing:
        return L"missing";
    case FirewallRuleHealth::Healthy:
        return L"healthy";
    case FirewallRuleHealth::Misconfigured:
        return L"misconfigured";
    case FirewallRuleHealth::Error:
        return L"error";
    }
    return L"unknown";
}

[[nodiscard]] int RunFirewallCommand(const CommandLine& command) {
    const auto executable = WindowsFirewallManager::CurrentExecutablePath();
    if (executable.empty()) {
        std::wcerr << L"Unable to resolve the AirPlayWin executable path.\n";
        return 20;
    }
    const auto rules = WindowsFirewallManager::CoreRuleSpecs(
        executable, command.raop_port, command.airplay_port);
    bool succeeded = true;
    if (command.remove_firewall_rules) {
        for (const auto& rule : rules) {
            std::uint32_t error = ERROR_SUCCESS;
            const bool removed = WindowsFirewallManager::Remove(rule.name, error);
            succeeded = succeeded && removed;
            std::wcout << L"Firewall rule \"" << rule.name << L"\": "
                       << (removed ? L"removed/not present" : L"remove failed")
                       << L", error=0x" << std::hex << error << std::dec << L"\n";
        }
        return succeeded ? 0 : 21;
    }
    if (command.install_firewall_rules) {
        for (const auto& rule : rules) {
            std::uint32_t error = ERROR_SUCCESS;
            const bool installed = WindowsFirewallManager::Install(rule, error);
            succeeded = succeeded && installed;
            std::wcout << L"Firewall rule \"" << rule.name << L"\": "
                       << (installed ? L"installed" : L"install failed") << L", error=0x"
                       << std::hex << error << std::dec << L"\n";
        }
        return succeeded ? 0 : 22;
    }
    for (const auto& rule : rules) {
        const auto status = WindowsFirewallManager::Query(rule);
        const bool healthy = status.health == FirewallRuleHealth::Healthy;
        succeeded = succeeded && healthy;
        std::wcout << L"Firewall rule \"" << rule.name << L"\": "
                   << FirewallHealthText(status.health);
        if (status.health == FirewallRuleHealth::Healthy ||
            status.health == FirewallRuleHealth::Misconfigured) {
            std::wcout << L", private_only="
                       << (status.private_profile_only ? L"yes" : L"no")
                       << L", program="
                       << (status.executable_matches ? L"match" : L"mismatch");
        }
        std::wcout << L", error=0x" << std::hex << status.last_error << std::dec << L"\n";
    }
    return succeeded ? 0 : 23;
}

void ListDevices() {
    const auto devices = WindowsAudioDeviceEnumerator::EnumerateRenderDevices();
    if (devices.empty()) {
        std::wcout << L"No Windows render endpoints were found.\n";
        return;
    }
    for (const auto& device : devices) {
        std::wcout << (device.is_default ? L"[default] " : L"          ")
                   << device.friendly_name << L"\n  id: " << device.id << L"\n  state: 0x"
                   << std::hex << device.state << std::dec << L"\n";
    }
}

void ListNetworkInterfaces(const bool include_virtual_interfaces) {
    const auto interfaces =
        WindowsNetworkInterfaceEnumerator::Enumerate(include_virtual_interfaces);
    if (interfaces.empty()) {
        std::wcout << L"No eligible IPv4 multicast interfaces were found.\n";
        return;
    }
    for (const auto& interface_info : interfaces) {
        std::wcout << L"[if " << interface_info.ipv4_interface_index << L"] "
                   << interface_info.friendly_name << L"\n  IPv4: "
                   << interface_info.ipv4_address_text << L"\n  description: "
                   << interface_info.description
                   << (interface_info.is_virtual ? L"\n  virtual: yes\n" : L"\n  virtual: no\n");
    }
}

[[nodiscard]] int RunLoopbackAnalysis(const CommandLine& command) {
    airplaywin::app::WaveCapture capture;
    std::wstring error;
    if (!airplaywin::app::ReadWaveCapture(command.loopback_capture_path, capture, error)) {
        std::wcerr << L"Loopback capture read failed: " << error << L".\n";
        return 25;
    }
    airplaywin::audio::LoopbackLatencyConfig config{
        .sample_rate = capture.sample_rate,
        .channel_count = capture.channel_count,
        .reference_channel = command.loopback_stimulus_frame.has_value()
                                 ? std::optional<std::uint16_t>{}
                                 : std::optional<std::uint16_t>{
                                       static_cast<std::uint16_t>(
                                           command.loopback_reference_channel)},
        .known_stimulus_frame = command.loopback_stimulus_frame,
        .output_channel = static_cast<std::uint16_t>(command.loopback_output_channel),
        .onset_threshold = command.loopback_onset_threshold,
        .maximum_latency_milliseconds =
            command.loopback_maximum_latency_milliseconds,
    };
    const auto result = airplaywin::audio::LoopbackLatencyAnalyzer::Analyze(
        capture.interleaved_samples, config);
    std::wcout << L"loopback_status=" << LoopbackStatusName(result.status)
               << L", sample_rate=" << capture.sample_rate
               << L", channels=" << capture.channel_count
               << L", reference_frame=" << result.reference_onset_frame
               << L", output_frame=" << result.output_onset_frame
               << L", latency_frames=" << result.latency_frames
               << L", latency_us=" << result.latency_microseconds
               << L", reference_peak=" << result.reference_peak
               << L", output_peak=" << result.output_peak << L"\n";
    return result.Succeeded() ? 0 : 26;
}

[[nodiscard]] int RunGroupSyncAnalysis(const CommandLine& command) {
    airplaywin::app::WaveCapture capture;
    std::wstring error;
    if (!airplaywin::app::ReadWaveCapture(command.group_sync_capture_path, capture,
                                           error)) {
        std::wcerr << L"Group capture read failed: " << error << L".\n";
        return 30;
    }

    auto channels = command.group_sync_channels;
    if (channels.empty()) {
        const auto member_count = std::min<std::uint16_t>(capture.channel_count, 4U);
        channels.reserve(member_count);
        for (std::uint16_t channel = 0U; channel < member_count; ++channel) {
            channels.push_back(channel);
        }
    }

    airplaywin::group::GroupSyncWaveformConfig config{
        .sample_rate = capture.sample_rate,
        .channel_count = capture.channel_count,
        .onset_threshold = command.loopback_onset_threshold,
        .minimum_pulse_gap_milliseconds =
            command.group_sync_minimum_pulse_gap_milliseconds,
        .maximum_absolute_skew_milliseconds =
            command.group_sync_maximum_skew_milliseconds,
    };
    config.members.reserve(channels.size());
    for (std::size_t index = 0U; index < channels.size(); ++index) {
        config.members.push_back({
            .member_id = static_cast<airplaywin::group::GroupMemberId>(index + 1U),
            .channel = channels[index],
        });
    }
    const auto result = airplaywin::group::GroupSyncAnalyzer::AnalyzeWaveform(
        capture.interleaved_samples, config);
    std::wcout << L"group_sync_status=" << GroupSyncStatusName(result.status)
               << L", targets=" << (result.meets_targets ? L"pass" : L"fail")
               << L", sample_rate=" << capture.sample_rate
               << L", members=" << channels.size()
               << L", pulses(reference/complete/incomplete)="
               << result.reference_pulses << L"/" << result.complete_pulses << L"/"
               << result.incomplete_pulses
               << L", skew_us(p50/p95/p99/max)="
               << result.group_skew.p50_microseconds << L"/"
               << result.group_skew.p95_microseconds << L"/"
               << result.group_skew.p99_microseconds << L"/"
               << result.group_skew.maximum_microseconds
               << L", first_alignment_us=" << result.first_alignment_microseconds
               << L", correlation(refined/fallback)="
               << result.correlation_refinements << L"/" << result.onset_fallbacks
               << L"\n";
    for (std::size_t index = 0U; index < result.members.size(); ++index) {
        const auto& member = result.members[index];
        std::wcout << L"  member=" << member.member_id
                   << L", channel=" << channels[index]
                   << L", matched=" << member.matched_pulses
                   << L", offset_us(mean/min/max)="
                   << member.mean_offset_microseconds << L"/"
                   << member.minimum_offset_microseconds << L"/"
                   << member.maximum_offset_microseconds
                   << L", abs_offset_us(p50/p95/p99)="
                   << member.absolute_offset.p50_microseconds << L"/"
                   << member.absolute_offset.p95_microseconds << L"/"
                   << member.absolute_offset.p99_microseconds
                   << L", correlation_mean=" << member.mean_correlation << L"\n";
    }
    if (!result.Succeeded()) {
        return 31;
    }
    return result.meets_targets ? 0 : 32;
}

[[nodiscard]] bool SubmitBlock(WindowsAudioEngine& engine,
                               TestSignalGenerator& generator,
                               std::span<float> block,
                               const std::uint32_t frames) {
    generator.Fill(block, frames);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.Submit(block, frames, QpcClock::Now())) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

void PrintDiagnostics(const WindowsAudioEngine& engine) {
    const auto diagnostics = engine.Diagnostics();
    std::wcout << L"depth=" << diagnostics.current_buffer_depth_frames
               << L" frames, underruns=" << diagnostics.underrun_count
               << L", rendered=" << diagnostics.rendered_frames
               << L", stale_epoch=" << diagnostics.dropped_stale_epoch_buffers
               << L", latency_est=" << diagnostics.output_latency_microseconds / 1'000U
               << L" ms, epoch=" << diagnostics.current_audio_epoch
               << L"\n  output: requested="
               << OutputModeName(diagnostics.requested_output_mode)
               << L", active=" << OutputModeName(diagnostics.active_output_mode)
               << L", client=" << AudioClientPathName(diagnostics.audio_client_path)
               << L", endpoint_format="
               << EndpointSampleFormatName(diagnostics.endpoint_sample_format)
               << L", low_latency="
               << (diagnostics.low_latency_active ? L"active" : L"inactive")
               << L", fallback="
               << (diagnostics.output_mode_fallback ? L"yes" : L"no")
               << L", fallback_error=0x" << std::hex
               << diagnostics.output_mode_fallback_error
               << L", output_error=0x" << diagnostics.last_output_error << std::dec
               << L", session_volume="
               << (diagnostics.session_volume_active ? L"active/" : L"fallback/")
               << diagnostics.session_volume_scalar
               << L", session_volume_error=0x" << std::hex
               << diagnostics.session_volume_error << std::dec
               << L", recovering="
               << (diagnostics.output_recovering ? L"yes" : L"no")
               << L", recovery(attempt/success/fail)="
               << diagnostics.device_recovery_attempts << L"/"
               << diagnostics.device_recovery_successes << L"/"
               << diagnostics.device_recovery_failures
               << L", wakeups=" << diagnostics.render_wakeup_count
               << L", exclusive_start_timeouts="
               << diagnostics.exclusive_start_timeouts
               << L", endpoint_buffer=" << diagnostics.endpoint_buffer_frames
               << L", period=" << diagnostics.engine_period_frames
               << L", queue_target=" << diagnostics.queue_target_frames << L" frames"
               << L"\n  latency_breakdown_us: queue="
               << diagnostics.software_queue_latency_microseconds
               << L", padding=" << diagnostics.endpoint_padding_latency_microseconds
               << L", engine=" << diagnostics.engine_latency_microseconds
               << L", calibration="
               << diagnostics.endpoint_calibration_offset_microseconds
               << L"\n  transition=" << TransitionStateName(diagnostics.transition_state)
               << L", requests=" << diagnostics.transition_requests
               << L", fade_in=" << diagnostics.fade_in_events
               << L", fade_out=" << diagnostics.fade_out_events
               << L", safe_mute=" << diagnostics.safe_mute_events
               << L", hard_resync=" << diagnostics.hard_resync_events
               << L", underrun_transitions=" << diagnostics.underrun_transition_events
               << L"\n  click_pop: analyzed_frames=" << diagnostics.click_pop_analyzed_frames
               << L", events=" << diagnostics.click_pop_events
               << L", max_step=" << diagnostics.click_pop_maximum_step
               << L", recent_peak=" << diagnostics.click_pop_recent_peak
               << L", last_event_frame=" << diagnostics.click_pop_last_event_frame
               << L"\n  numeric: invalid=" << diagnostics.invalid_numeric_samples
               << L", clipped=" << diagnostics.clipped_samples
               << L", dc_events=" << diagnostics.dc_offset_events << L"\n"
               << std::flush;
}

[[nodiscard]] int RunProbe(const CommandLine& command) {
    const AudioFormat format{.sample_rate = command.sample_rate, .channel_count = 2U};
    WindowsAudioEngine engine{WasapiOutputOptions{
        .device_id = command.device_id,
        .follow_default_device = command.device_id.empty(),
        .output_mode = command.exclusive_audio ? AudioOutputMode::Exclusive
                                               : AudioOutputMode::Shared,
        .low_latency = command.low_latency,
        .allow_shared_fallback = !command.strict_exclusive,
        .ring_capacity_milliseconds = 500U,
        .target_queue_milliseconds = command.exclusive_audio
                                         ? 10U
                                         : (command.low_latency ? 5U : 15U),
        .endpoint_calibration_offset_microseconds =
            command.endpoint_calibration_offset_microseconds,
    }};
    if (!engine.Open(format)) {
        const auto diagnostics = engine.Diagnostics();
        std::wcerr << L"Failed to open the selected WASAPI render endpoint, error=0x"
                   << std::hex << diagnostics.last_output_error << std::dec << L".\n";
        return 2;
    }
    const auto output_configuration = engine.Diagnostics();
    const auto block_frames = command.low_latency &&
                                      output_configuration.engine_period_frames != 0U
                                  ? output_configuration.engine_period_frames
                                  : 240U;
    std::vector<float> samples(static_cast<std::size_t>(block_frames) *
                               format.channel_count);
    TestSignalGenerator generator{format, command.signal,
                                  command.signal == TestSignal::LatencyPulse ? 0.5F : 0.2F};
    const auto configured_prefill = command.low_latency
                                        ? output_configuration.queue_target_frames
                                        : 2U * block_frames;
    const auto prefill_blocks = command.low_latency
                                    ? std::max(1U, (configured_prefill + block_frames - 1U) /
                                                       block_frames)
                                    : 2U;
    for (std::uint32_t block = 0U; block < prefill_blocks; ++block) {
        if (!SubmitBlock(engine, generator, samples, block_frames)) {
            std::wcerr << L"Failed to prefill the WASAPI queue.\n";
            PrintDiagnostics(engine);
            engine.Close();
            return 3;
        }
    }
    if (!engine.Start()) {
        std::wcerr << L"Failed to start WASAPI event-driven rendering.\n";
        engine.Close();
        return 4;
    }

    if (command.transition_cycles != 0U) {
        for (std::uint32_t cycle = 0U; cycle < command.transition_cycles; ++cycle) {
            for (std::uint32_t block = 0U; block < 20U; ++block) {
                if (!SubmitBlock(engine, generator, samples, block_frames)) {
                    std::wcerr << L"PCM submission stalled during transition cycle " << cycle
                               << L".\n";
                    engine.Close();
                    return 5;
                }
            }
            engine.Pause();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            engine.Resume();
            for (std::uint32_t block = 0U; block < 4U; ++block) {
                if (!SubmitBlock(engine, generator, samples, block_frames)) {
                    std::wcerr << L"PCM submission stalled after resume in cycle " << cycle
                               << L".\n";
                    engine.Close();
                    return 6;
                }
            }
            engine.SetVolume(0.0F);
            for (std::uint32_t block = 0U; block < 4U; ++block) {
                if (!SubmitBlock(engine, generator, samples, block_frames)) {
                    std::wcerr << L"PCM submission stalled during volume-down in cycle " << cycle
                               << L".\n";
                    engine.Close();
                    return 7;
                }
            }
            engine.SetVolume(1.0F);
            for (std::uint32_t block = 0U; block < 4U; ++block) {
                if (!SubmitBlock(engine, generator, samples, block_frames)) {
                    std::wcerr << L"PCM submission stalled during volume-up in cycle " << cycle
                               << L".\n";
                    engine.Close();
                    return 8;
                }
            }
            switch (cycle % 4U) {
            case 0U:
                engine.Flush();
                break;
            case 1U:
                engine.Seek();
                break;
            case 2U:
                engine.HardResync();
                break;
            case 3U:
                engine.ReplaceSender();
                break;
            default:
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            for (std::uint32_t block = 0U; block < 5U; ++block) {
                if (!SubmitBlock(engine, generator, samples, block_frames)) {
                    std::wcerr << L"PCM submission stalled after timeline reset in cycle "
                               << cycle
                               << L".\n";
                    engine.Close();
                    return 9;
                }
            }
            engine.Stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            for (std::uint32_t block = 0U; block < prefill_blocks; ++block) {
                if (!SubmitBlock(engine, generator, samples, block_frames)) {
                    std::wcerr << L"PCM prefill stalled before restart in cycle " << cycle
                               << L".\n";
                    engine.Close();
                    return 10;
                }
            }
            if (!engine.Start()) {
                std::wcerr << L"Restart failed during transition cycle " << cycle << L".\n";
                engine.Close();
                return 11;
            }
            if ((cycle + 1U) % 10U == 0U) {
                std::wcout << L"Completed transition cycles: " << cycle + 1U << L"\n";
                PrintDiagnostics(engine);
            }
        }
    } else {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(command.duration_seconds);
        const auto diagnostics_interval =
            std::chrono::seconds(command.diagnostics_interval_seconds);
        auto next_diagnostics = std::chrono::steady_clock::now() + diagnostics_interval;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!SubmitBlock(engine, generator, samples, block_frames)) {
                std::wcerr << L"PCM submission stalled.\n";
                PrintDiagnostics(engine);
                engine.Close();
                return 12;
            }
            if (std::chrono::steady_clock::now() >= next_diagnostics) {
                PrintDiagnostics(engine);
                next_diagnostics += diagnostics_interval;
            }
        }
    }

    engine.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    PrintDiagnostics(engine);
    engine.Close();
    return 0;
}

void PrintDiscoveryDiagnostics(const airplaywin::discovery::DiscoveryDiagnostics& diagnostics) {
    std::wcout << L"name=\"" << diagnostics.advertised_name << L"\", interfaces="
               << diagnostics.active_interface_count << L", registrations="
               << diagnostics.registered_service_count << L", generation="
               << diagnostics.publication_generation << L", network_changes="
               << diagnostics.network_change_count << L", conflicts="
               << diagnostics.name_conflict_count << L", failures="
               << diagnostics.registration_failure_count << L", last_error="
               << diagnostics.last_error << L"\n"
               << std::flush;
}

[[nodiscard]] int RunDiscovery(const CommandLine& command) {
    airplaywin::discovery::DiscoveryConfig config{
        .device_name = command.discovery_name,
        .device_id = command.discovery_device_id,
        .raop_port = command.raop_port,
        .airplay_port = command.airplay_port,
        .advertise_raop = true,
        .advertise_airplay = !command.classic_raop,
        .classic_raop = command.classic_raop,
        .include_virtual_interfaces = command.include_virtual_interfaces,
    };
    if (airplaywin::discovery::IsZeroDeviceId(config.device_id)) {
        config.device_id = WindowsNetworkInterfaceEnumerator::SystemDeviceId();
    }
    const std::wstring advertised_name =
        airplaywin::discovery::NormalizeDeviceName(config.device_name);
    const auto services = airplaywin::discovery::BuildAirPlayServiceRecords(
        config, advertised_name, WindowsNetworkInterfaceEnumerator::LocalHostName());
    std::wcout << L"Device ID: "
               << airplaywin::discovery::FormatDeviceId(config.device_id, true) << L"\n";
    for (const auto& service : services) {
        std::wcout << L"Advertise: " << airplaywin::discovery::BuildServiceFqdn(service)
                   << L" port " << service.port << L"\n";
        for (const auto& property : service.txt_properties) {
            std::wcout << L"  " << property.key << L"=" << property.value << L"\n";
        }
    }

    WindowsDiscoveryService discovery;
    if (!discovery.Start(config)) {
        std::wcerr << L"Failed to start Windows native DNS-SD; error="
                   << discovery.Diagnostics().last_error << L".\n";
        return 13;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(command.duration_seconds);
    const auto interval = std::chrono::seconds(command.diagnostics_interval_seconds);
    auto next_diagnostics = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::chrono::steady_clock::now() >= next_diagnostics) {
            PrintDiscoveryDiagnostics(discovery.Diagnostics());
            next_diagnostics += interval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const auto final_diagnostics = discovery.Diagnostics();
    discovery.Stop();
    PrintDiscoveryDiagnostics(final_diagnostics);
    return final_diagnostics.registered_service_count == 0U ? 14 : 0;
}

void PrintControlDiagnostics(
    const airplaywin::protocol::ControlDiagnostics& control,
    const airplaywin::windows::network::IocpTcpServerDiagnostics& raop,
    const airplaywin::windows::network::IocpTcpServerDiagnostics& airplay,
    const airplaywin::windows::audio::AudioStreamSinkDiagnostics& audio,
    std::uint64_t& last_request_trace_sequence) {
    const auto& transport = control.transport;
    std::wcout << L"control: sessions=" << control.sessions.active_connections
               << L", requests=" << control.received_requests
               << L", parse_errors=" << control.parse_errors
               << L", rejected=" << control.rejected_requests
               << L", unsupported=" << control.unsupported_requests
               << L", pairing_requests=" << control.pairing_requests
               << L", disconnects(peer/request/idle/transport/shutdown/protocol)="
               << control.peer_disconnects << L"/" << control.requested_disconnects << L"/"
               << control.idle_disconnects << L"/" << control.transport_disconnects << L"/"
               << control.shutdown_disconnects << L"/" << control.protocol_disconnects
               << L"\n"
               << L"  RAOP " << raop.bound_port << L": active=" << raop.active_connections
               << L", accepted=" << raop.accepted_connections << L", rx="
               << raop.received_bytes << L", tx=" << raop.sent_bytes
               << L", timeouts=" << raop.idle_timeouts << L", errors="
               << raop.transport_errors << L"\n"
               << L"  AirPlay " << airplay.bound_port << L": active="
               << airplay.active_connections << L", accepted="
               << airplay.accepted_connections << L", rx=" << airplay.received_bytes
               << L", tx=" << airplay.sent_bytes << L", timeouts="
               << airplay.idle_timeouts << L", errors=" << airplay.transport_errors << L"\n"
               << std::flush;
    for (const auto& request : control.recent_requests) {
        if (request.sequence <= last_request_trace_sequence) {
            continue;
        }
        const std::wstring method{request.method.begin(), request.method.end()};
        const std::wstring target{request.target.begin(), request.target.end()};
        const std::wstring protocol{request.protocol.begin(), request.protocol.end()};
        const std::wstring content_type{request.content_type.begin(), request.content_type.end()};
        std::wcout << L"  request[" << request.sequence << L"] connection="
                   << request.connection_id << L", " << method << L" " << target << L" "
                   << protocol << L", content_type="
                   << (content_type.empty() ? L"none" : content_type)
                   << L", body_bytes=" << request.body_bytes
                   << L", response=" << request.response_status
                   << L", apple_challenge/response="
                   << (request.apple_challenge_present ? L"yes" : L"no") << L"/"
                   << (request.apple_response_sent ? L"yes" : L"no") << L"\n";
        last_request_trace_sequence =
            (std::max)(last_request_trace_sequence, request.sequence);
    }
    std::wcout
               << L"  RTP " << transport.server_audio_port
               << L": configured=" << (transport.configured ? L"yes" : L"no")
               << L", recording=" << (transport.recording ? L"yes" : L"no")
               << L", packets=" << transport.datagrams_received
               << L", retransmitted=" << transport.retransmitted_packets
               << L", stale_timeline=" << transport.timeline_rejected_packets
               << L", decoded=" << transport.decoded_packets
               << L", concealed=" << transport.concealed_packets
               << L", resample(in/out/insert/drop)="
               << transport.resampled_input_frames << L"/"
               << transport.resampled_output_frames << L"/"
               << transport.drift_inserted_frames << L"/"
               << transport.drift_dropped_frames
               << L", timing_hard_resync="
               << transport.timing_hard_resync_requests
               << L", unmapped_dropped_frames="
               << transport.timing_unmapped_dropped_frames
               << L", lost=" << transport.jitter_buffer.lost_packets
               << L", late=" << transport.jitter_buffer.late_packets
               << L", duplicate=" << transport.jitter_buffer.duplicate_packets
               << L", reordered=" << transport.jitter_buffer.reordered_packets
               << L", jitter_us="
               << transport.jitter_buffer.interarrival_jitter_microseconds
               << L", adaptive="
               << (transport.jitter_buffer.adaptive_enabled ? L"yes" : L"no")
               << L", jitter_state="
               << AdaptiveJitterStateName(transport.jitter_buffer.adaptive_state)
               << L", target(min/current/max)="
               << transport.jitter_buffer.minimum_target_packets << L"/"
               << transport.jitter_buffer.target_packets << L"/"
               << transport.jitter_buffer.maximum_target_packets
               << L", p95/p99_us=" << transport.jitter_buffer.jitter_p95_microseconds
               << L"/" << transport.jitter_buffer.jitter_p99_microseconds
               << L", target_up/down="
               << transport.jitter_buffer.target_increase_events << L"/"
               << transport.jitter_buffer.target_decrease_events
               << L", downstream_underruns="
               << transport.jitter_buffer.downstream_underruns
               << L", protocol_latency(frames/us)="
               << transport.protocol_latency_frames << L"/"
               << transport.protocol_latency_microseconds
               << L", readiness(clock/decode/ready)="
               << (transport.jitter_buffer.timing_locked ? L"yes" : L"no") << L"/"
               << (transport.jitter_buffer.decode_margin_sufficient ? L"yes" : L"no")
               << L"/"
               << (transport.jitter_buffer.low_latency_conditions_ready ? L"yes" : L"no")
               << L", packet_us(avg/max)="
               << transport.packet_processing_average_microseconds << L"/"
               << transport.packet_processing_maximum_microseconds
               << L", decoder_error(count/stage/code)=" << transport.decoder_errors
               << L"/" << static_cast<unsigned int>(
                               transport.last_decoder_failure_point)
               << L"/0x" << std::hex << transport.last_decoder_platform_error
               << std::dec
               << L", decode_us(avg/max)="
               << transport.decode_processing_average_microseconds << L"/"
               << transport.decode_processing_maximum_microseconds
               << L", decode_budget_miss=" << transport.decode_budget_miss_count
               << L", receiver_latency_us(jitter/scheduled/output/estimate)="
               << transport.jitter_reserve_microseconds << L"/"
               << transport.scheduled_reserve_microseconds << L"/"
               << transport.output_path_latency_microseconds << L"/"
               << transport.receiver_added_latency_estimate_microseconds
               << L", timeline_resets=" << transport.timeline_resets
               << L", anchor_seq="
               << (transport.has_sequence_anchor
                       ? std::to_wstring(transport.sequence_anchor)
                       : std::wstring{L"none"})
               << L", anchor_rtptime="
               << (transport.has_timestamp_anchor
                       ? std::to_wstring(transport.timestamp_anchor)
                       : std::wstring{L"none"})
               << L"\n  timing: enabled=" << (transport.timing.enabled ? L"yes" : L"no")
               << L", locked=" << (transport.timing.locked ? L"yes" : L"no")
               << L", generation=" << transport.timing.generation
               << L", buffer_us=" << transport.timing.target_buffer_microseconds
               << L", mapped=" << transport.timing.mapped_packets
               << L", late=" << transport.timing.late_mappings
               << L", max_late_us=" << transport.timing.maximum_lateness_microseconds
               << L", anchor_remote=" << transport.timing.anchor_remote_time
               << L", anchor_target_qpc=" << transport.timing.anchor_target_qpc
               << L", last_remote=" << transport.timing.last_remote_time
               << L", last_target_qpc=" << transport.timing.last_target_qpc
               << L", phase(anchor/active/epoch)="
               << (transport.timing.phase_anchor_available ? L"yes" : L"no") << L"/"
               << (transport.timing.absolute_phase_active ? L"yes" : L"no") << L"/"
               << transport.timing.phase_session_epoch
               << L", phase_remote_ptp_ns="
               << transport.timing.phase_remote_ptp_nanoseconds
               << L", phase_map(ok/fail)="
               << transport.timing.absolute_phase_mappings << L"/"
               << transport.timing.phase_mapping_failures
               << L", endpoint_offset_us="
               << transport.timing.endpoint_latency_offset_microseconds
               << L", servo_state="
               << ClockServoStateName(transport.timing.servo.state)
               << L", master=0x" << std::hex
               << transport.timing.servo.source_clock_identity << std::dec
               << L", offset_us=" << transport.timing.servo.offset_microseconds
               << L", drift_ppm=" << transport.timing.servo.drift_ppm
               << L", correction=" << transport.timing.servo.rate_correction
               << L", rtt_us=" << transport.timing.servo.rtt_microseconds
               << L", uncertainty_us="
               << transport.timing.servo.uncertainty_microseconds
               << L", sync_age_us="
               << transport.timing.servo.last_sync_age_microseconds
               << L", samples(ok/reject/outlier)="
               << transport.timing.servo.accepted_samples << L"/"
               << transport.timing.servo.rejected_samples << L"/"
               << transport.timing.servo.outlier_samples
               << L", holdover/relock/resync="
               << transport.timing.servo.holdover_events << L"/"
               << transport.timing.servo.relock_events << L"/"
               << transport.timing.servo.hard_resync_events
               << L", error=" << transport.timing.last_error
               << L"\n"
               << L"  audio: configured=" << (audio.configured ? L"yes" : L"no")
               << L", started=" << (audio.started ? L"yes" : L"no")
               << L", accepted_frames=" << audio.accepted_frames
               << L", rejected_frames=" << audio.rejected_frames
               << L", scheduled_frames=" << audio.scheduled_frames
               << L", schedule_late_frames=" << audio.late_scheduled_frames
               << L", schedule_wait_us=" << audio.scheduling_wait_microseconds
               << L", schedule_max_late_us="
               << audio.maximum_schedule_lateness_microseconds
               << L", last_target_qpc=" << audio.last_target_qpc
               << L", queue_frames=" << audio.output.current_buffer_depth_frames
               << L", underruns=" << audio.output.underrun_count
               << L", latency_us=" << audio.output.output_latency_microseconds
               << L", output=" << OutputModeName(audio.output.active_output_mode)
               << L", client=" << AudioClientPathName(audio.output.audio_client_path)
               << L", endpoint_format="
               << EndpointSampleFormatName(audio.output.endpoint_sample_format)
               << L", period_frames=" << audio.output.engine_period_frames
               << L", queue_target_frames=" << audio.output.queue_target_frames
               << L", low_latency="
               << (audio.output.low_latency_active ? L"active" : L"inactive")
               << L", fallback="
               << (audio.output.output_mode_fallback ? L"yes" : L"no")
               << L", session_volume="
               << (audio.output.session_volume_active ? L"active/" : L"fallback/")
               << audio.output.session_volume_scalar
               << L", session_volume_error=0x" << std::hex
               << audio.output.session_volume_error << std::dec
               << L", wakeups=" << audio.output.render_wakeup_count
               << L", exclusive_start_timeouts="
               << audio.output.exclusive_start_timeouts
               << L", transition=" << TransitionStateName(audio.output.transition_state)
               << L", transition_requests=" << audio.output.transition_requests
               << L", fade(in/out)=" << audio.output.fade_in_events << L"/"
               << audio.output.fade_out_events
               << L", safe_mute=" << audio.output.safe_mute_events
               << L", hard_resync=" << audio.output.hard_resync_events
               << L", underrun_transitions="
               << audio.output.underrun_transition_events
               << L", click_pop(events/max/recent)=" << audio.output.click_pop_events
               << L"/" << audio.output.click_pop_maximum_step << L"/"
               << audio.output.click_pop_recent_peak
               << L", numeric(invalid/clipped/dc)="
               << audio.output.invalid_numeric_samples << L"/"
               << audio.output.clipped_samples << L"/"
               << audio.output.dc_offset_events
               << L", device_recovering="
               << (audio.output.output_recovering ? L"yes" : L"no")
               << L", device_switches=" << audio.output.device_switch_events
               << L", recovery(attempt/success/fail)="
               << audio.output.device_recovery_attempts << L"/"
               << audio.output.device_recovery_successes << L"/"
               << audio.output.device_recovery_failures << L", output_error=0x" << std::hex
               << audio.output.last_output_error << std::dec << L"\n"
               << std::flush;
}

void PrintPtpDiagnostics(
    const airplaywin::windows::timing::WindowsPtpTimingServiceDiagnostics& diagnostics) {
    const auto& clock = diagnostics.clock;
    const auto& servo = clock.servo;
    const std::wstring multicast_interface{diagnostics.multicast_interface_address.begin(),
                                           diagnostics.multicast_interface_address.end()};
    std::wcout << L"  PTP: running=" << (diagnostics.running ? L"yes" : L"no")
               << L", ports=" << diagnostics.event_port << L"/"
               << diagnostics.general_port
               << L", multicast="
               << (diagnostics.multicast_joined ? L"joined" : L"not-joined")
               << L", interface=" << multicast_interface
               << L", datagrams(event/general)=" << diagnostics.event_datagrams << L"/"
               << diagnostics.general_datagrams
               << L", messages(sync/follow/announce/invalid)="
               << clock.sync_messages << L"/" << clock.follow_up_messages << L"/"
               << clock.announce_messages << L"/" << clock.invalid_datagrams
               << L", domain=" << static_cast<unsigned>(clock.domain_number)
               << L", state=" << ClockServoStateName(servo.state)
               << L", master=0x" << std::hex << servo.source_clock_identity << std::dec
               << L", samples(ok/reject/outlier)=" << servo.accepted_samples << L"/"
               << servo.rejected_samples << L"/" << servo.outlier_samples
               << L", offset_us=" << servo.offset_microseconds
               << L", drift_ppm=" << servo.drift_ppm
               << L", correction=" << servo.rate_correction
               << L", uncertainty_us=" << servo.uncertainty_microseconds
               << L", sync_age_us=" << servo.last_sync_age_microseconds
               << L", holdover/relock/resync=" << servo.holdover_events << L"/"
               << servo.relock_events << L"/" << servo.hard_resync_events
               << L", receive_errors=" << diagnostics.receive_errors
               << L", error=0x" << std::hex << diagnostics.last_error << std::dec
               << L"\n" << std::flush;
}

void PrintRecoveryDiagnostics(
    const airplaywin::lifecycle::RecoveryDiagnostics& recovery,
    const airplaywin::windows::system::PowerEventDiagnostics& power) {
    std::wcout << L"  lifecycle: power_monitor=" << (power.running ? L"running" : L"stopped")
               << L", suspend=" << recovery.suspend_events
               << L", resume=" << recovery.resume_events
               << L", generation=" << recovery.recovery_generation
               << L", recovery(attempt/success/fail)=" << recovery.recovery_attempts << L"/"
               << recovery.recovery_successes << L"/" << recovery.recovery_failures
               << L", last_error=0x" << std::hex << recovery.last_error
               << L", monitor_error=0x" << power.last_error << std::dec << L"\n"
               << std::flush;
}

[[nodiscard]] int RunControlServer(const CommandLine& command) {
    if (command.raop_port == command.airplay_port) {
        std::wcerr << L"RAOP and AirPlay control ports must be different.\n";
        return 15;
    }
    auto receiver_device_id = command.discovery_device_id;
    if (airplaywin::discovery::IsZeroDeviceId(receiver_device_id)) {
        receiver_device_id = WindowsNetworkInterfaceEnumerator::SystemDeviceId();
    }
    WindowsRaopCryptoProvider raop_crypto{receiver_device_id};
    if (command.classic_raop && !raop_crypto.Available()) {
        std::wcerr << L"Classic RAOP cryptographic provider initialization failed.\n";
        return 26;
    }
    airplaywin::crypto::OpenSessionAuthenticator authenticator;
    WindowsAudioStreamSink audio_sink{WasapiOutputOptions{
        .device_id = command.device_id,
        .follow_default_device = command.device_id.empty(),
        .output_mode = command.exclusive_audio ? AudioOutputMode::Exclusive
                                               : AudioOutputMode::Shared,
        .low_latency = command.low_latency,
        .allow_shared_fallback = !command.strict_exclusive,
        .ring_capacity_milliseconds = 500U,
        .target_queue_milliseconds = command.exclusive_audio
                                         ? 10U
                                         : (command.low_latency ? 5U : 30U),
        .endpoint_calibration_offset_microseconds =
            command.endpoint_calibration_offset_microseconds,
    }, command.exclusive_audio ? 10U : (command.low_latency ? 5U : 20U),
       command.low_latency ? 15U : 30U};
    std::shared_ptr<airplaywin::timing::PtpClockDomain> ptp_clock_domain;
    std::unique_ptr<WindowsPtpTimingService> ptp_service;
    if (command.experimental_ptp_timing) {
        ptp_clock_domain = std::make_shared<airplaywin::timing::PtpClockDomain>(
            airplaywin::timing::ClockServoConfig{
                .local_clock_frequency = QpcClock::Frequency(),
            });
        ptp_service = std::make_unique<WindowsPtpTimingService>(ptp_clock_domain);
    }
    WindowsRtpTransportController media_transport{
        audio_sink,
        {.enable_adaptive_jitter = command.low_latency,
         .enable_buffered_timing = command.experimental_buffered_timing,
         .enable_ptp_timing = command.experimental_ptp_timing,
         .buffered_timing_milliseconds = command.buffered_timing_milliseconds,
         .ptp_clock_domain = ptp_clock_domain,
         .endpoint_latency_offset_microseconds =
             command.endpoint_calibration_offset_microseconds}};
    airplaywin::protocol::AirPlayControlService control{
        authenticator, airplaywin::session::ActiveSessionPolicy::RejectNew,
        airplaywin::protocol::ParserLimits{}, &media_transport,
        command.classic_raop ? &raop_crypto : nullptr};
    IocpTcpServer raop_server{control};
    IocpTcpServer airplay_server{control};
    airplaywin::discovery::DiscoveryConfig discovery_config{
        .device_name = command.discovery_name,
        .device_id = receiver_device_id,
        .raop_port = command.raop_port,
        .airplay_port = command.airplay_port,
        .advertise_raop = true,
        .advertise_airplay = !command.classic_raop,
        .classic_raop = command.classic_raop,
        .include_virtual_interfaces = command.include_virtual_interfaces,
    };
    WindowsDiscoveryService discovery;
    auto stop_services = [&]() noexcept {
        discovery.Stop();
        airplay_server.Stop();
        raop_server.Stop();
        if (ptp_service) {
            ptp_service->Stop();
        }
    };
    auto start_services = [&]() -> int {
        if (ptp_service && !ptp_service->Start({
                               .multicast_interface_address =
                                   command.ptp_interface_address})) {
            return 25;
        }
        if (!raop_server.Start({.bind_address = "0.0.0.0", .port = command.raop_port})) {
            if (ptp_service) {
                ptp_service->Stop();
            }
            return 16;
        }
        if (!airplay_server.Start(
                {.bind_address = "0.0.0.0", .port = command.airplay_port})) {
            raop_server.Stop();
            if (ptp_service) {
                ptp_service->Stop();
            }
            return 17;
        }
        if (!discovery.Start(discovery_config)) {
            airplay_server.Stop();
            raop_server.Stop();
            if (ptp_service) {
                ptp_service->Stop();
            }
            return 18;
        }
        return 0;
    };
    auto runtime_error = [&]() noexcept {
        const auto discovery_error = discovery.Diagnostics().last_error;
        const auto airplay_error = airplay_server.Diagnostics().last_error;
        const auto raop_error = raop_server.Diagnostics().last_error;
        const auto ptp_error = ptp_service ? ptp_service->Diagnostics().last_error : 0U;
        return ptp_error != 0U
                   ? ptp_error
                   : discovery_error != 0U
                   ? discovery_error
                   : (airplay_error != 0U ? airplay_error : raop_error);
    };

    const auto start_result = start_services();
    if (start_result != 0) {
        std::wcerr << L"Failed to start receiver services; stage=" << start_result
                   << L", error=" << runtime_error() << L".\n";
        return start_result;
    }

    WindowsPowerEventMonitor power_monitor;
    if (!power_monitor.Start()) {
        std::wcerr << L"Power notification registration failed; receiver remains available, "
                      L"error="
                   << power_monitor.Diagnostics().last_error << L".\n";
    }
    airplaywin::lifecycle::RecoveryCoordinator recovery;
    ScopedConsoleControlHandler console_control;
    bool services_running = true;
    auto next_recovery_attempt = std::chrono::steady_clock::time_point{};

    std::wcout << L"Control receiver ready: RAOP TCP " << command.raop_port
               << L", AirPlay TCP " << command.airplay_port
               << L". Authentication is open; unencrypted RTP/L16 development transport is "
                  L"enabled. Buffered timing experiment: "
               << (command.experimental_buffered_timing ? L"on" : L"off")
               << (command.experimental_buffered_timing
                       ? L" (local RTP-to-QPC anchor only; no PTP).\n"
                       : L". ")
               << L"PTP timing experiment: "
               << (command.experimental_ptp_timing
                       ? L"on (receive-only UDP 319/320 servo; an external group "
                         L"coordinator may publish the S11 RTP/PTP phase anchor).\n"
                       : L"off.\n");
    const auto deadline =
        command.run_until_stopped
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() +
                  std::chrono::seconds(command.duration_seconds);
    const auto interval = std::chrono::seconds(command.diagnostics_interval_seconds);
    auto next_diagnostics = std::chrono::steady_clock::now();
    std::uint64_t last_request_trace_sequence = 0U;
    const auto lifecycle_suspend_at =
        command.lifecycle_smoke ? std::chrono::steady_clock::now() + std::chrono::seconds{1}
                                : std::chrono::steady_clock::time_point::max();
    auto lifecycle_resume_at = std::chrono::steady_clock::time_point::max();
    bool lifecycle_smoke_suspended = false;
    bool lifecycle_smoke_resumed = false;
    while (std::chrono::steady_clock::now() < deadline &&
           !ShutdownRequestedFlag().load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (!lifecycle_smoke_suspended && now >= lifecycle_suspend_at) {
            power_monitor.RecordPowerBroadcast(PBT_APMSUSPEND);
            lifecycle_smoke_suspended = true;
            lifecycle_resume_at = now + std::chrono::seconds{1};
        }
        if (lifecycle_smoke_suspended && !lifecycle_smoke_resumed &&
            now >= lifecycle_resume_at) {
            power_monitor.RecordPowerBroadcast(PBT_APMRESUMEAUTOMATIC);
            lifecycle_smoke_resumed = true;
        }
        while (const auto event = power_monitor.Poll()) {
            if (*event == PowerLifecycleEvent::Suspend) {
                recovery.OnSuspend();
                if (services_running) {
                    stop_services();
                    services_running = false;
                }
                std::wcout << L"Receiver suspended: sessions and audio were safely stopped.\n";
            } else {
                recovery.OnResume();
                next_recovery_attempt = now;
                std::wcout << L"Receiver resume observed: recovery scheduled.\n";
            }
        }
        if (!services_running && recovery.RecoveryRequired() &&
            now >= next_recovery_attempt) {
            const auto recovery_result = start_services();
            services_running = recovery_result == 0;
            recovery.RecordRecoveryResult(services_running, runtime_error());
            next_recovery_attempt = now + std::chrono::seconds{1};
            if (services_running) {
                std::wcout << L"Receiver services recovered and DNS-SD was republished.\n";
            }
        }
        if (now >= next_diagnostics) {
            PrintControlDiagnostics(control.Diagnostics(), raop_server.Diagnostics(),
                                    airplay_server.Diagnostics(), audio_sink.Diagnostics(),
                                    last_request_trace_sequence);
            PrintRecoveryDiagnostics(recovery.Diagnostics(), power_monitor.Diagnostics());
            if (ptp_service) {
                PrintPtpDiagnostics(ptp_service->Diagnostics());
            }
            next_diagnostics += interval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    const auto raop_diagnostics = raop_server.Diagnostics();
    const auto airplay_diagnostics = airplay_server.Diagnostics();
    const auto control_diagnostics = control.Diagnostics();
    PrintControlDiagnostics(control_diagnostics, raop_diagnostics, airplay_diagnostics,
                            audio_sink.Diagnostics(), last_request_trace_sequence);
    PrintRecoveryDiagnostics(recovery.Diagnostics(), power_monitor.Diagnostics());
    if (ptp_service) {
        PrintPtpDiagnostics(ptp_service->Diagnostics());
    }
    recovery.Stop();
    power_monitor.Stop();
    if (services_running) {
        stop_services();
    }
    return raop_diagnostics.transport_errors == 0U &&
                   airplay_diagnostics.transport_errors == 0U
               ? 0
               : 19;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
    static_cast<void>(SetConsoleOutputCP(CP_UTF8));
    static_cast<void>(_setmode(_fileno(stdout), _O_U8TEXT));
    static_cast<void>(_setmode(_fileno(stderr), _O_U8TEXT));
    CommandLine command;
    if (!ParseCommandLine(argc, argv, command)) {
        PrintUsage();
        return 1;
    }
    if (command.show_help) {
        PrintUsage();
        return 0;
    }
    const bool calibration_command = command.save_endpoint_calibration ||
                                     command.show_endpoint_calibration ||
                                     command.clear_endpoint_calibration;
    if (calibration_command) {
        return RunEndpointCalibrationCommand(command);
    }
    LoadPersistedEndpointCalibration(command);
    const bool firewall_command = command.firewall_status || command.install_firewall_rules ||
                                  command.remove_firewall_rules;
    if (firewall_command) {
        return RunFirewallCommand(command);
    }
    if (command.analyze_loopback) {
        return RunLoopbackAnalysis(command);
    }
    if (command.analyze_group_sync) {
        return RunGroupSyncAnalysis(command);
    }
    if (command.list_devices || (!command.play && !command.discover && !command.serve &&
                                 !command.list_network_interfaces &&
                                 !command.analyze_loopback &&
                                 !command.analyze_group_sync)) {
        ListDevices();
    }
    if (command.list_network_interfaces) {
        ListNetworkInterfaces(command.include_virtual_interfaces);
    }
    if (command.play) {
        return RunProbe(command);
    }
    if (command.serve) {
        SingleInstanceGuard instance{L"Local\\AirPlayWin.Core"};
        if (!instance.Acquired()) {
            std::wcerr << (instance.AlreadyRunning()
                               ? L"Another AirPlayWin receiver instance is already running.\n"
                               : L"Unable to create the AirPlayWin single-instance guard.\n");
            return 24;
        }
        return RunControlServer(command);
    }
    if (command.discover) {
        return RunDiscovery(command);
    }
    if (!command.list_devices && !command.list_network_interfaces) {
        PrintUsage();
    }
    return 0;
}
