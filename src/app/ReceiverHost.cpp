#include "app/ReceiverHost.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#include "core/crypto/OpenSessionAuthenticator.h"
#include "core/discovery/AirPlayServiceRecords.h"
#include "platform/windows/crypto/WindowsRaopCryptoProvider.h"
#include "platform/windows/network/WindowsDiscoveryService.h"
#include "platform/windows/network/WindowsNetworkInterfaceEnumerator.h"
#include "platform/windows/network/WindowsRtpTransportController.h"
#include "platform/windows/timing/QpcClock.h"

namespace airplaywin::app {

using airplaywin::crypto::OpenSessionAuthenticator;
using airplaywin::windows::audio::WasapiOutputOptions;
using airplaywin::windows::audio::WindowsAudioStreamSink;
using airplaywin::windows::crypto::WindowsRaopCryptoProvider;
using airplaywin::windows::network::IocpTcpServer;
using airplaywin::windows::network::WindowsDiscoveryService;
using airplaywin::windows::network::WindowsNetworkInterfaceEnumerator;
using airplaywin::windows::network::WindowsRtpTransportController;
using airplaywin::windows::network::WindowsRtpTransportOptions;
using airplaywin::windows::system::PowerLifecycleEvent;
using airplaywin::windows::system::WindowsPowerEventMonitor;
using airplaywin::windows::timing::QpcClock;
using airplaywin::windows::timing::WindowsPtpTimingService;

namespace {

[[nodiscard]] discovery::DeviceId ResolveDeviceId(
    const discovery::DeviceId requested) {
    return discovery::IsZeroDeviceId(requested)
               ? WindowsNetworkInterfaceEnumerator::SystemDeviceId()
               : requested;
}

}  // namespace

class ReceiverHost::Impl final {
public:
    explicit Impl(ReceiverHostConfig config) : config_(std::move(config)) {}

    ~Impl() {
        Stop();
    }

    [[nodiscard]] bool Start() {
        std::scoped_lock lock{mutex_};
        if (running_.load(std::memory_order_acquire) ||
            maintenance_thread_.joinable()) {
            return true;
        }
        const auto receiver_device_id = ResolveDeviceId(config_.device_id);
        raop_crypto_ =
            std::make_unique<WindowsRaopCryptoProvider>(receiver_device_id);
        if (config_.classic_raop && !raop_crypto_->Available()) {
            last_error_.store(26U, std::memory_order_release);
            PublishStateLocked(false, 26U, L"Classic RAOP cryptographic provider failed.");
            return false;
        }

        audio_sink_ = std::make_unique<WindowsAudioStreamSink>(
            WasapiOutputOptions{
                .device_id = config_.output_device_id,
                .follow_default_device = config_.output_device_id.empty(),
                .output_mode = config_.exclusive_audio ? audio::AudioOutputMode::Exclusive
                                                       : audio::AudioOutputMode::Shared,
                .low_latency = config_.low_latency,
                .allow_shared_fallback = !config_.strict_exclusive,
                .ring_capacity_milliseconds = 500U,
                .target_queue_milliseconds = config_.exclusive_audio
                                                 ? 10U
                                                 : (config_.low_latency ? 5U : 30U),
                .endpoint_calibration_offset_microseconds =
                    config_.endpoint_calibration_offset_microseconds,
            },
            config_.exclusive_audio ? 10U : (config_.low_latency ? 5U : 20U),
            config_.low_latency ? 15U : 30U);

        if (config_.experimental_ptp_timing) {
            ptp_clock_domain_ = std::make_shared<timing::PtpClockDomain>(
                timing::ClockServoConfig{
                    .local_clock_frequency = QpcClock::Frequency(),
                });
            ptp_service_ = std::make_unique<WindowsPtpTimingService>(ptp_clock_domain_);
        }
        media_transport_ = std::make_unique<WindowsRtpTransportController>(
            *audio_sink_,
            WindowsRtpTransportOptions{
                .enable_adaptive_jitter = config_.low_latency,
                .enable_buffered_timing = config_.experimental_buffered_timing,
                .enable_ptp_timing = config_.experimental_ptp_timing,
                .buffered_timing_milliseconds = config_.buffered_timing_milliseconds,
                .ptp_clock_domain = ptp_clock_domain_,
                .endpoint_latency_offset_microseconds =
                    config_.endpoint_calibration_offset_microseconds});
        control_ = std::make_unique<protocol::AirPlayControlService>(
            authenticator_, session::ActiveSessionPolicy::RejectNew,
            protocol::ParserLimits{}, media_transport_.get(),
            config_.classic_raop ? raop_crypto_.get() : nullptr);
        raop_server_ = std::make_unique<IocpTcpServer>(*control_);
        airplay_server_ = std::make_unique<IocpTcpServer>(*control_);
        discovery_ = std::make_unique<WindowsDiscoveryService>();
        discovery_config_ = discovery::DiscoveryConfig{
            .device_name = config_.device_name,
            .device_id = receiver_device_id,
            .raop_port = config_.raop_port,
            .airplay_port = config_.airplay_port,
            .advertise_raop = true,
            .advertise_airplay = !config_.classic_raop,
            .classic_raop = config_.classic_raop,
            .include_virtual_interfaces = config_.include_virtual_interfaces,
        };

        const auto start_result = StartServicesLocked();
        if (start_result != 0) {
            const auto error = RuntimeErrorLocked();
            last_error_.store(static_cast<std::uint32_t>(start_result),
                              std::memory_order_release);
            DestroyServicesLocked();
            PublishStateLocked(false, static_cast<std::uint32_t>(start_result),
                               L"Unable to start receiver services; error=0x" +
                                   std::to_wstring(error));
            return false;
        }

        power_monitor_ = std::make_unique<WindowsPowerEventMonitor>();
        static_cast<void>(power_monitor_->Start());

        services_running_ = true;
        consecutive_recovery_failures_ = 0U;
        stop_requested_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        last_error_.store(0U, std::memory_order_release);
        try {
            maintenance_thread_ = std::thread{[this] { MaintenanceLoop(); }};
        } catch (...) {
            running_.store(false, std::memory_order_release);
            StopServicesLocked();
            DestroyServicesLocked();
            PublishStateLocked(false, 3U, L"Unable to create the maintenance thread.");
            return false;
        }
        PublishStateLocked(true, 0U, {});
        return true;
    }

    void Stop() noexcept {
        if (!running_.exchange(false, std::memory_order_acq_rel) &&
            !maintenance_thread_.joinable()) {
            return;
        }
        stop_requested_.store(true, std::memory_order_release);
        if (maintenance_thread_.joinable()) {
            maintenance_thread_.join();
        }
        std::scoped_lock lock{mutex_};
        DestroyServicesLocked();
        PublishStateLocked(false, 0U, L"Receiver stopped.");
    }

    [[nodiscard]] bool Running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint32_t LastError() const noexcept {
        return last_error_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool SetDeviceName(const std::wstring_view name) noexcept {
        if (name.empty()) {
            return false;
        }
        std::scoped_lock lock{mutex_};
        config_.device_name = name;
        if (discovery_ && services_running_) {
            discovery_->Stop();
            discovery_config_.device_name = name;
            const bool republished = discovery_->Start(discovery_config_);
            if (!republished) {
                last_error_.store(discovery_->Diagnostics().last_error,
                                  std::memory_order_release);
            }
            return republished;
        }
        discovery_config_.device_name = name;
        return true;
    }

    [[nodiscard]] bool SetVolume(const float linear_gain) noexcept {
        if (!running_.load(std::memory_order_acquire) || !audio_sink_) {
            return false;
        }
        std::scoped_lock lock{mutex_};
        audio_sink_->SetVolume(linear_gain);
        return true;
    }

    void PostSimulatedPowerEvent(const bool suspend) noexcept {
        if (suspend) {
            simulated_suspend_pending_.store(true, std::memory_order_release);
        } else {
            simulated_resume_pending_.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] ReceiverSnapshot Snapshot() const {
        std::scoped_lock lock{mutex_};
        ReceiverSnapshot snapshot;
        snapshot.running = running_.load(std::memory_order_acquire);
        snapshot.last_error = last_error_.load(std::memory_order_acquire);
        if (control_) {
            snapshot.control = control_->Diagnostics();
            snapshot.transport = snapshot.control.transport;
            snapshot.sessions = control_->Sessions();
        }
        if (discovery_) {
            snapshot.discovery = discovery_->Diagnostics();
        }
        if (power_monitor_) {
            snapshot.power = power_monitor_->Diagnostics();
        }
        snapshot.recovery = recovery_.Diagnostics();
        if (ptp_service_) {
            snapshot.ptp_enabled = true;
            snapshot.ptp = ptp_service_->Diagnostics();
        }
        if (audio_sink_) {
            snapshot.audio = audio_sink_->Diagnostics();
        }
        if (raop_server_) {
            snapshot.raop_server = raop_server_->Diagnostics();
        }
        if (airplay_server_) {
            snapshot.airplay_server = airplay_server_->Diagnostics();
        }
        return snapshot;
    }

    [[nodiscard]] ReceiverHostConfig Config() const {
        std::scoped_lock lock{mutex_};
        return config_;
    }

    void SetDiagnosticsCallback(DiagnosticsCallback callback) {
        std::scoped_lock lock{callback_mutex_};
        diagnostics_callback_ = std::move(callback);
    }

    void SetStateCallback(StateCallback callback) {
        std::scoped_lock lock{callback_mutex_};
        state_callback_ = std::move(callback);
    }

private:
    [[nodiscard]] int StartServicesLocked() noexcept {
        if (ptp_service_ &&
            !ptp_service_->Start(
                {.multicast_interface_address = config_.ptp_interface_address})) {
            return 25;
        }
        if (!raop_server_->Start(
                {.bind_address = "0.0.0.0", .port = config_.raop_port})) {
            if (ptp_service_) {
                ptp_service_->Stop();
            }
            return 16;
        }
        if (!config_.classic_raop &&
            !airplay_server_->Start(
                {.bind_address = "0.0.0.0", .port = config_.airplay_port})) {
            raop_server_->Stop();
            if (ptp_service_) {
                ptp_service_->Stop();
            }
            return 17;
        }
        if (!discovery_->Start(discovery_config_)) {
            if (!config_.classic_raop) {
                airplay_server_->Stop();
            }
            raop_server_->Stop();
            if (ptp_service_) {
                ptp_service_->Stop();
            }
            return 18;
        }
        return 0;
    }

    void StopServicesLocked() noexcept {
        if (discovery_) {
            discovery_->Stop();
        }
        if (airplay_server_) {
            airplay_server_->Stop();
        }
        if (raop_server_) {
            raop_server_->Stop();
        }
        if (ptp_service_) {
            ptp_service_->Stop();
        }
    }

    void DestroyServicesLocked() noexcept {
        StopServicesLocked();
        if (power_monitor_) {
            power_monitor_->Stop();
        }
        recovery_.Stop();
        power_monitor_.reset();
        discovery_.reset();
        airplay_server_.reset();
        raop_server_.reset();
        control_.reset();
        media_transport_.reset();
        ptp_service_.reset();
        ptp_clock_domain_.reset();
        audio_sink_.reset();
        raop_crypto_.reset();
        services_running_ = false;
    }

    [[nodiscard]] std::uint32_t RuntimeErrorLocked() const noexcept {
        const auto discovery_error = discovery_ ? discovery_->Diagnostics().last_error : 0U;
        const auto airplay_error = !config_.classic_raop && airplay_server_
                                       ? airplay_server_->Diagnostics().last_error
                                       : 0U;
        const auto raop_error = raop_server_ ? raop_server_->Diagnostics().last_error : 0U;
        const auto ptp_error = ptp_service_ ? ptp_service_->Diagnostics().last_error : 0U;
        return ptp_error != 0U
                   ? ptp_error
                   : discovery_error != 0U
                       ? discovery_error
                       : (airplay_error != 0U ? airplay_error : raop_error);
    }

    void MaintenanceLoop() noexcept {
        auto next_diagnostics = std::chrono::steady_clock::now();
        auto next_recovery_attempt = std::chrono::steady_clock::time_point{};
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            // Real platform notifications and the simulated test events
            // share the same suspend/resume handling below.
            if (simulated_suspend_pending_.exchange(false, std::memory_order_acq_rel)) {
                HandleSuspend(now);
            }
            if (simulated_resume_pending_.exchange(false, std::memory_order_acq_rel)) {
                recovery_.OnResume();
                next_recovery_attempt = now;
            }
            if (power_monitor_) {
                while (const auto event = power_monitor_->Poll()) {
                    if (*event == PowerLifecycleEvent::Suspend) {
                        HandleSuspend(now);
                    } else {
                        recovery_.OnResume();
                        next_recovery_attempt = now;
                    }
                }
            }
            bool recovered = false;
            {
                std::scoped_lock lock{mutex_};
                if (!services_running_ && recovery_.RecoveryRequired() &&
                    now >= next_recovery_attempt) {
                    const auto result = StartServicesLocked();
                    services_running_ = result == 0;
                    recovery_.RecordRecoveryResult(services_running_, RuntimeErrorLocked());
                    if (!services_running_) {
                        ++consecutive_recovery_failures_;
                        const auto shift = std::min(consecutive_recovery_failures_, 5U);
                        const auto delay_seconds = std::min<std::uint32_t>(30U, 1U << shift);
                        next_recovery_attempt =
                            now + std::chrono::seconds{delay_seconds};
                    } else {
                        consecutive_recovery_failures_ = 0U;
                        recovered = true;
                    }
                }
            }
            if (recovered) {
                PublishState(true, 0U, L"Receiver services recovered.");
            }
            if (now >= next_diagnostics) {
                PublishDiagnostics();
                next_diagnostics +=
                    std::chrono::seconds{std::max(config_.diagnostics_interval_seconds, 1U)};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        std::scoped_lock lock{mutex_};
        StopServicesLocked();
        services_running_ = false;
    }

    void HandleSuspend(const std::chrono::steady_clock::time_point) noexcept {
        recovery_.OnSuspend();
        {
            std::scoped_lock lock{mutex_};
            if (services_running_) {
                StopServicesLocked();
                services_running_ = false;
            }
        }
        PublishState(false, 0U, L"Receiver suspended.");
    }

    void PublishDiagnostics() {
        const auto snapshot = Snapshot();
        DiagnosticsCallback callback;
        {
            std::scoped_lock lock{callback_mutex_};
            callback = diagnostics_callback_;
        }
        if (callback) {
            callback(snapshot);
        }
    }

    void PublishState(const bool running, const std::uint32_t error,
                      const std::wstring message) {
        StateCallback callback;
        {
            std::scoped_lock lock{callback_mutex_};
            callback = state_callback_;
        }
        if (callback) {
            callback(running, error, message);
        }
    }

    void PublishStateLocked(const bool running, const std::uint32_t error,
                            const std::wstring message) {
        StateCallback callback;
        {
            std::scoped_lock lock{callback_mutex_};
            callback = state_callback_;
        }
        if (callback) {
            callback(running, error, message);
        }
    }

    ReceiverHostConfig config_;
    mutable std::mutex mutex_;
    mutable std::mutex callback_mutex_;
    DiagnosticsCallback diagnostics_callback_{};
    StateCallback state_callback_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> simulated_suspend_pending_{false};
    std::atomic<bool> simulated_resume_pending_{false};
    std::atomic<std::uint32_t> last_error_{0U};
    bool services_running_{false};
    std::uint32_t consecutive_recovery_failures_{0U};

    discovery::DiscoveryConfig discovery_config_{};
    crypto::OpenSessionAuthenticator authenticator_{};
    std::unique_ptr<WindowsRaopCryptoProvider> raop_crypto_;
    std::unique_ptr<WindowsAudioStreamSink> audio_sink_;
    std::shared_ptr<timing::PtpClockDomain> ptp_clock_domain_;
    std::unique_ptr<WindowsPtpTimingService> ptp_service_;
    std::unique_ptr<WindowsRtpTransportController> media_transport_;
    std::unique_ptr<protocol::AirPlayControlService> control_;
    std::unique_ptr<IocpTcpServer> raop_server_;
    std::unique_ptr<IocpTcpServer> airplay_server_;
    std::unique_ptr<WindowsDiscoveryService> discovery_;
    std::unique_ptr<WindowsPowerEventMonitor> power_monitor_;
    lifecycle::RecoveryCoordinator recovery_{};
    std::thread maintenance_thread_;
};

ReceiverHost::ReceiverHost(ReceiverHostConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

ReceiverHost::~ReceiverHost() = default;

bool ReceiverHost::Start() {
    return impl_ != nullptr && impl_->Start();
}

void ReceiverHost::Stop() noexcept {
    if (impl_ != nullptr) {
        impl_->Stop();
    }
}

bool ReceiverHost::Running() const noexcept {
    return impl_ != nullptr && impl_->Running();
}

std::uint32_t ReceiverHost::LastError() const noexcept {
    return impl_ != nullptr ? impl_->LastError() : 1U;
}

bool ReceiverHost::SetDeviceName(const std::wstring_view name) noexcept {
    return impl_ != nullptr && impl_->SetDeviceName(name);
}

bool ReceiverHost::SetVolume(const float linear_gain) noexcept {
    return impl_ != nullptr && impl_->SetVolume(linear_gain);
}

ReceiverSnapshot ReceiverHost::Snapshot() const {
    return impl_ != nullptr ? impl_->Snapshot() : ReceiverSnapshot{};
}

ReceiverHostConfig ReceiverHost::Config() const {
    return impl_ != nullptr ? impl_->Config() : ReceiverHostConfig{};
}

void ReceiverHost::SetDiagnosticsCallback(DiagnosticsCallback callback) {
    if (impl_ != nullptr) {
        impl_->SetDiagnosticsCallback(std::move(callback));
    }
}

void ReceiverHost::SetStateCallback(StateCallback callback) {
    if (impl_ != nullptr) {
        impl_->SetStateCallback(std::move(callback));
    }
}

void ReceiverHost::PostSimulatedPowerEvent(const bool suspend) noexcept {
    if (impl_ != nullptr) {
        impl_->PostSimulatedPowerEvent(suspend);
    }
}

}  // namespace airplaywin::app
