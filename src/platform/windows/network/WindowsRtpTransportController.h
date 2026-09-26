#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "core/audio/IAudioFrameSink.h"
#include "core/timing/PtpClockDomain.h"
#include "core/transport/IAudioTransportController.h"
#include "core/transport/IAudioStream.h"
#include "core/transport/IUdpDatagramHandler.h"
#include "platform/windows/network/IocpUdpReceiver.h"

namespace airplaywin::windows::network {

struct WindowsRtpTransportOptions final {
    std::string bind_address{"0.0.0.0"};
    std::size_t jitter_capacity_packets{256U};
    std::size_t jitter_target_packets{4U};
    bool enable_adaptive_jitter{false};
    std::size_t jitter_minimum_packets{2U};
    std::size_t jitter_maximum_packets{16U};
    std::uint32_t jitter_stable_window_packets{128U};
    std::uint32_t jitter_recovery_window_packets{32U};
    bool enable_buffered_timing{false};
    bool enable_ptp_timing{false};
    std::uint32_t buffered_timing_milliseconds{120U};
    std::shared_ptr<airplaywin::timing::PtpClockDomain> ptp_clock_domain{};
};

class WindowsRtpTransportController final
    : public airplaywin::transport::IAudioTransportController {
public:
    explicit WindowsRtpTransportController(
        airplaywin::audio::IAudioFrameSink& sink,
        WindowsRtpTransportOptions options = {});
    ~WindowsRtpTransportController() override;

    WindowsRtpTransportController(const WindowsRtpTransportController&) = delete;
    WindowsRtpTransportController& operator=(const WindowsRtpTransportController&) = delete;

    [[nodiscard]] airplaywin::transport::AudioTransportSetupResult Setup(
        const airplaywin::transport::AudioTransportSetupRequest& request) override;
    [[nodiscard]] bool Record(
        airplaywin::transport::ConnectionId connection_id,
        const airplaywin::transport::AudioTimelineAnchor& anchor) override;
    [[nodiscard]] bool Pause(
        airplaywin::transport::ConnectionId connection_id) noexcept override;
    [[nodiscard]] bool Resume(
        airplaywin::transport::ConnectionId connection_id,
        const airplaywin::transport::AudioTimelineAnchor& anchor) noexcept override;
    [[nodiscard]] bool Flush(
        airplaywin::transport::ConnectionId connection_id,
        const airplaywin::transport::AudioTimelineAnchor& anchor) noexcept override;
    void SetVolume(airplaywin::transport::ConnectionId connection_id,
                   float linear_gain) noexcept override;
    void Teardown(airplaywin::transport::ConnectionId connection_id) noexcept override;
    [[nodiscard]] airplaywin::transport::AudioTransportDiagnostics Diagnostics()
        const noexcept override;

private:
    class CountingHandler : public airplaywin::transport::IUdpDatagramHandler {
    public:
        void OnDatagram(std::span<const std::byte> datagram,
                        const airplaywin::transport::DatagramEndpoint& source,
                        std::int64_t arrival_time_nanoseconds) noexcept override;
        [[nodiscard]] std::uint64_t Count() const noexcept;
        void Reset() noexcept;

    private:
        std::atomic<std::uint64_t> count_{0U};
    };

    class RetransmitHandler final : public CountingHandler {
    public:
        void OnDatagram(std::span<const std::byte> datagram,
                        const airplaywin::transport::DatagramEndpoint& source,
                        std::int64_t arrival_time_nanoseconds) noexcept override;
        void SetStream(airplaywin::transport::IAudioStream* stream) noexcept;

    private:
        std::atomic<airplaywin::transport::IAudioStream*> stream_{nullptr};
    };

    void TeardownLocked() noexcept;

    airplaywin::audio::IAudioFrameSink& sink_;
    WindowsRtpTransportOptions options_{};
    mutable std::mutex mutex_{};
    airplaywin::transport::ConnectionId connection_id_{0U};
    RetransmitHandler control_handler_{};
    CountingHandler timing_handler_{};
    std::unique_ptr<airplaywin::transport::IAudioStream> stream_{};
    std::unique_ptr<IocpUdpReceiver> audio_receiver_{};
    std::unique_ptr<IocpUdpReceiver> control_receiver_{};
    std::unique_ptr<IocpUdpReceiver> timing_receiver_{};
    std::uint32_t last_error_{0U};
};

}  // namespace airplaywin::windows::network
