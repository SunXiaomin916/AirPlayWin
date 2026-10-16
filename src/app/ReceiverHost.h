#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/discovery/DiscoveryTypes.h"
#include "core/group/GroupTypes.h"
#include "core/lifecycle/RecoveryCoordinator.h"
#include "core/protocol/AirPlayControlService.h"
#include "core/session/SessionManager.h"
#include "core/timing/PtpClockDomain.h"
#include "platform/windows/audio/WindowsAudioStreamSink.h"
#include "platform/windows/network/IocpTcpServer.h"
#include "platform/windows/system/WindowsPowerEventMonitor.h"
#include "platform/windows/timing/WindowsPtpTimingService.h"

namespace airplaywin::app {

// Receiver composition shared by the console CLI and the graphical app.
// Every feature the command line exposes maps onto this configuration; the
// GUI serializes the same fields from its settings page.
struct ReceiverHostConfig final {
    std::wstring device_name{L"AirPlayWin"};
    discovery::DeviceId device_id{};
    std::wstring output_device_id{};
    bool low_latency{false};
    bool exclusive_audio{false};
    bool strict_exclusive{false};
    std::uint16_t raop_port{5'000U};
    std::uint16_t airplay_port{7'000U};
    bool classic_raop{true};
    bool include_virtual_interfaces{false};
    bool experimental_buffered_timing{false};
    std::uint32_t buffered_timing_milliseconds{120U};
    bool experimental_ptp_timing{false};
    std::string ptp_interface_address{"0.0.0.0"};
    std::int64_t endpoint_calibration_offset_microseconds{0};
    std::filesystem::path log_file_path{};
    std::uint32_t diagnostics_interval_seconds{1U};
};

// Aggregated, thread-safe snapshot of every diagnostics surface. The UI
// serializes this to JSON for the settings/diagnostics pages.
struct ReceiverSnapshot final {
    bool running{false};
    std::uint32_t last_error{0U};
    protocol::ControlDiagnostics control{};
    std::vector<session::SessionSnapshot> sessions{};
    discovery::DiscoveryDiagnostics discovery{};
    lifecycle::RecoveryDiagnostics recovery{};
    windows::system::PowerEventDiagnostics power{};
    windows::timing::WindowsPtpTimingServiceDiagnostics ptp{};
    bool ptp_enabled{false};
    windows::audio::AudioStreamSinkDiagnostics audio{};
    transport::AudioTransportDiagnostics transport{};
    windows::network::IocpTcpServerDiagnostics raop_server{};
    windows::network::IocpTcpServerDiagnostics airplay_server{};
};

class ReceiverHost final {
public:
    using DiagnosticsCallback = std::function<void(const ReceiverSnapshot&)>;
    using StateCallback = std::function<void(bool running, std::uint32_t error,
                                             std::wstring message)>;

    explicit ReceiverHost(ReceiverHostConfig config);
    ~ReceiverHost();

    ReceiverHost(const ReceiverHost&) = delete;
    ReceiverHost& operator=(const ReceiverHost&) = delete;

    // Starts every service (optional PTP, control TCP, DNS-SD) and the
    // maintenance loop. Returns false with LastError() set on failure.
    [[nodiscard]] bool Start();
    // Asynchronous, ordered shutdown: services stop from the maintenance
    // thread so the call never blocks the UI.
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::uint32_t LastError() const noexcept;

    // Live configuration changes (applied without a full restart where the
    // platform allows it). Device name changes republish DNS-SD.
    [[nodiscard]] bool SetDeviceName(std::wstring_view name) noexcept;
    // Sets the local output volume (Windows per-app session volume when
    // available, sample-ramped gain otherwise). Works with and without an
    // active sender session.
    [[nodiscard]] bool SetVolume(float linear_gain) noexcept;

    [[nodiscard]] ReceiverSnapshot Snapshot() const;
    [[nodiscard]] ReceiverHostConfig Config() const;

    void SetDiagnosticsCallback(DiagnosticsCallback callback);
    void SetStateCallback(StateCallback callback);
    // Test hook for the console lifecycle smoke: posts a synthetic
    // suspend/resume power event consumed by the maintenance loop.
    void PostSimulatedPowerEvent(bool suspend) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace airplaywin::app
