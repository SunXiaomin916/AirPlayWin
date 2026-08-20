#include <Windows.h>
#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/audio/TestSignalGenerator.h"
#include "core/crypto/OpenSessionAuthenticator.h"
#include "core/discovery/AirPlayServiceRecords.h"
#include "core/lifecycle/RecoveryCoordinator.h"
#include "core/protocol/AirPlayControlService.h"
#include "platform/windows/audio/WindowsAudioDeviceEnumerator.h"
#include "platform/windows/audio/WindowsAudioEngine.h"
#include "platform/windows/audio/WindowsAudioStreamSink.h"
#include "platform/windows/network/WindowsDiscoveryService.h"
#include "platform/windows/network/IocpTcpServer.h"
#include "platform/windows/network/WindowsNetworkInterfaceEnumerator.h"
#include "platform/windows/network/WindowsRtpTransportController.h"
#include "platform/windows/system/SingleInstanceGuard.h"
#include "platform/windows/system/WindowsFirewallManager.h"
#include "platform/windows/system/WindowsPowerEventMonitor.h"
#include "platform/windows/timing/QpcClock.h"

namespace {

using airplaywin::audio::AudioFormat;
using airplaywin::audio::TestSignal;
using airplaywin::audio::TestSignalGenerator;
using airplaywin::windows::audio::WasapiOutputOptions;
using airplaywin::windows::audio::WindowsAudioDeviceEnumerator;
using airplaywin::windows::audio::WindowsAudioEngine;
using airplaywin::windows::audio::WindowsAudioStreamSink;
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
    std::uint32_t buffered_timing_milliseconds{120U};
    std::wstring device_id{};
    std::wstring discovery_name{L"AirPlayWin"};
    airplaywin::discovery::DeviceId discovery_device_id{};
    std::uint16_t raop_port{5'000U};
    std::uint16_t airplay_port{7'000U};
    bool include_virtual_interfaces{false};
    TestSignal signal{TestSignal::Sine440Hz};
    std::uint32_t duration_seconds{10U};
    std::uint32_t diagnostics_interval_seconds{1U};
    std::uint32_t transition_cycles{0U};
};

void PrintUsage() {
    std::wcout
        << L"AirPlayWin phase-7 buffered timing experiment\n\n"
        << L"  AirPlayWin --list-devices\n"
        << L"  AirPlayWin --list-network-interfaces [--include-virtual-interfaces]\n"
        << L"  AirPlayWin --play [--device <endpoint-id>] [--signal 440|1000|silence|impulse|sweep]\n"
        << L"             [--duration <seconds>] [--transition-cycles <count>]\n\n"
        << L"             [--diagnostics-interval <seconds>]\n\n"
        << L"  AirPlayWin --discover [--name <speaker-name>] [--device-id <AA:BB:CC:DD:EE:FF>]\n"
        << L"             [--raop-port <port>] [--airplay-port <port>] [--duration <seconds>]\n"
        << L"             [--include-virtual-interfaces]\n\n"
        << L"  AirPlayWin --serve [--name <speaker-name>] [--device-id <AA:BB:CC:DD:EE:FF>]\n"
        << L"             [--device <endpoint-id>]\n"
        << L"             [--raop-port <port>] [--airplay-port <port>] [--duration <seconds>]\n"
        << L"             [--run-until-stopped] [--diagnostics-interval <seconds>]\n"
        << L"             [--experimental-buffered-timing [--buffered-timing-ms <20..2000>]]\n"
        << L"             [--include-virtual-interfaces]\n\n"
        << L"  AirPlayWin --firewall-status [--raop-port <port>] [--airplay-port <port>]\n"
        << L"  AirPlayWin --install-firewall-rules [--raop-port <port>] [--airplay-port <port>]\n"
        << L"  AirPlayWin --remove-firewall-rules\n\n"
        << L"Examples:\n"
        << L"  AirPlayWin --play --signal 440 --duration 1800\n"
        << L"  AirPlayWin --play --device \"{endpoint-id}\" --transition-cycles 100\n"
        << L"  AirPlayWin --discover --name \"Living Room PC\" --duration 300\n"
        << L"  AirPlayWin --serve --name \"Living Room PC\" --duration 300\n";
}

[[nodiscard]] bool ParseUnsigned(const wchar_t* const text, std::uint32_t& value) {
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0' || parsed > UINT32_MAX) {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
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
        if (argument == L"--list-devices") {
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
        } else if (argument == L"--include-virtual-interfaces") {
            command.include_virtual_interfaces = true;
        } else if (argument == L"--signal" && index + 1 < argc) {
            if (!ParseSignal(argv[++index], command.signal)) {
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
    if (firewall_commands > 1U || (command.lifecycle_smoke && !command.serve) ||
        (command.run_until_stopped && !command.serve) ||
        (command.experimental_buffered_timing && !command.serve)) {
        return false;
    }
    if (firewall_commands != 0U &&
        (command.play || command.discover || command.serve || command.list_devices ||
         command.list_network_interfaces)) {
        return false;
    }
    return true;
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

[[nodiscard]] bool SubmitBlock(WindowsAudioEngine& engine,
                               TestSignalGenerator& generator,
                               std::span<float> block,
                               const std::uint32_t frames) {
    generator.Fill(block, frames);
    for (std::uint32_t retry = 0U; retry < 1'000U; ++retry) {
        if (engine.Submit(block, frames, QpcClock::Now())) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
               << L" ms, epoch=" << diagnostics.current_audio_epoch << L"\n"
               << std::flush;
}

[[nodiscard]] int RunProbe(const CommandLine& command) {
    const AudioFormat format{.sample_rate = 48'000U, .channel_count = 2U};
    WindowsAudioEngine engine{WasapiOutputOptions{
        .device_id = command.device_id,
        .follow_default_device = command.device_id.empty(),
        .ring_capacity_milliseconds = 500U,
        .target_queue_milliseconds = 15U,
    }};
    if (!engine.Open(format)) {
        std::wcerr << L"Failed to open the selected WASAPI render endpoint.\n";
        return 2;
    }
    constexpr std::uint32_t kBlockFrames = 240U;
    std::vector<float> samples(static_cast<std::size_t>(kBlockFrames) * format.channel_count);
    TestSignalGenerator generator{format, command.signal};
    for (std::uint32_t block = 0U; block < 2U; ++block) {
        if (!SubmitBlock(engine, generator, samples, kBlockFrames)) {
            std::wcerr << L"Failed to prefill the WASAPI queue.\n";
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
                if (!SubmitBlock(engine, generator, samples, kBlockFrames)) {
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
                if (!SubmitBlock(engine, generator, samples, kBlockFrames)) {
                    std::wcerr << L"PCM submission stalled after resume in cycle " << cycle
                               << L".\n";
                    engine.Close();
                    return 6;
                }
            }
            engine.SetVolume(0.0F);
            for (std::uint32_t block = 0U; block < 4U; ++block) {
                if (!SubmitBlock(engine, generator, samples, kBlockFrames)) {
                    std::wcerr << L"PCM submission stalled during volume-down in cycle " << cycle
                               << L".\n";
                    engine.Close();
                    return 7;
                }
            }
            engine.SetVolume(1.0F);
            for (std::uint32_t block = 0U; block < 4U; ++block) {
                if (!SubmitBlock(engine, generator, samples, kBlockFrames)) {
                    std::wcerr << L"PCM submission stalled during volume-up in cycle " << cycle
                               << L".\n";
                    engine.Close();
                    return 8;
                }
            }
            engine.Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            for (std::uint32_t block = 0U; block < 5U; ++block) {
                if (!SubmitBlock(engine, generator, samples, kBlockFrames)) {
                    std::wcerr << L"PCM submission stalled after flush in cycle " << cycle
                               << L".\n";
                    engine.Close();
                    return 9;
                }
            }
            engine.Stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            for (std::uint32_t block = 0U; block < 2U; ++block) {
                if (!SubmitBlock(engine, generator, samples, kBlockFrames)) {
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
            if (!SubmitBlock(engine, generator, samples, kBlockFrames)) {
                std::wcerr << L"PCM submission stalled.\n";
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
        .advertise_airplay = true,
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
    const airplaywin::windows::audio::AudioStreamSinkDiagnostics& audio) {
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
               << L"  RTP " << transport.server_audio_port
               << L": configured=" << (transport.configured ? L"yes" : L"no")
               << L", recording=" << (transport.recording ? L"yes" : L"no")
               << L", packets=" << transport.datagrams_received
               << L", retransmitted=" << transport.retransmitted_packets
               << L", stale_timeline=" << transport.timeline_rejected_packets
               << L", decoded=" << transport.decoded_packets
               << L", concealed=" << transport.concealed_packets
               << L", lost=" << transport.jitter_buffer.lost_packets
               << L", late=" << transport.jitter_buffer.late_packets
               << L", duplicate=" << transport.jitter_buffer.duplicate_packets
               << L", reordered=" << transport.jitter_buffer.reordered_packets
               << L", jitter_us="
               << transport.jitter_buffer.interarrival_jitter_microseconds
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
    airplaywin::crypto::OpenSessionAuthenticator authenticator;
    WindowsAudioStreamSink audio_sink{WasapiOutputOptions{
        .device_id = command.device_id,
        .follow_default_device = command.device_id.empty(),
    }};
    WindowsRtpTransportController media_transport{
        audio_sink,
        {.enable_buffered_timing = command.experimental_buffered_timing,
         .buffered_timing_milliseconds = command.buffered_timing_milliseconds}};
    airplaywin::protocol::AirPlayControlService control{
        authenticator, airplaywin::session::ActiveSessionPolicy::RejectNew,
        airplaywin::protocol::ParserLimits{}, &media_transport};
    IocpTcpServer raop_server{control};
    IocpTcpServer airplay_server{control};
    airplaywin::discovery::DiscoveryConfig discovery_config{
        .device_name = command.discovery_name,
        .device_id = command.discovery_device_id,
        .raop_port = command.raop_port,
        .airplay_port = command.airplay_port,
        .advertise_raop = true,
        .advertise_airplay = true,
        .include_virtual_interfaces = command.include_virtual_interfaces,
    };
    if (airplaywin::discovery::IsZeroDeviceId(discovery_config.device_id)) {
        discovery_config.device_id = WindowsNetworkInterfaceEnumerator::SystemDeviceId();
    }
    WindowsDiscoveryService discovery;
    auto stop_services = [&]() noexcept {
        discovery.Stop();
        airplay_server.Stop();
        raop_server.Stop();
    };
    auto start_services = [&]() -> int {
        if (!raop_server.Start({.bind_address = "0.0.0.0", .port = command.raop_port})) {
            return 16;
        }
        if (!airplay_server.Start(
                {.bind_address = "0.0.0.0", .port = command.airplay_port})) {
            raop_server.Stop();
            return 17;
        }
        if (!discovery.Start(discovery_config)) {
            airplay_server.Stop();
            raop_server.Stop();
            return 18;
        }
        return 0;
    };
    auto runtime_error = [&]() noexcept {
        const auto discovery_error = discovery.Diagnostics().last_error;
        const auto airplay_error = airplay_server.Diagnostics().last_error;
        const auto raop_error = raop_server.Diagnostics().last_error;
        return discovery_error != 0U
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
                       : L".\n");
    const auto deadline =
        command.run_until_stopped
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() +
                  std::chrono::seconds(command.duration_seconds);
    const auto interval = std::chrono::seconds(command.diagnostics_interval_seconds);
    auto next_diagnostics = std::chrono::steady_clock::now();
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
                                    airplay_server.Diagnostics(), audio_sink.Diagnostics());
            PrintRecoveryDiagnostics(recovery.Diagnostics(), power_monitor.Diagnostics());
            next_diagnostics += interval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    const auto raop_diagnostics = raop_server.Diagnostics();
    const auto airplay_diagnostics = airplay_server.Diagnostics();
    const auto control_diagnostics = control.Diagnostics();
    PrintControlDiagnostics(control_diagnostics, raop_diagnostics, airplay_diagnostics,
                            audio_sink.Diagnostics());
    PrintRecoveryDiagnostics(recovery.Diagnostics(), power_monitor.Diagnostics());
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
    const bool firewall_command = command.firewall_status || command.install_firewall_rules ||
                                  command.remove_firewall_rules;
    if (firewall_command) {
        return RunFirewallCommand(command);
    }
    if (command.list_devices || (!command.play && !command.discover && !command.serve &&
                                 !command.list_network_interfaces)) {
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
