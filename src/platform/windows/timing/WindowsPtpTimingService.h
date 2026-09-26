#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/timing/PtpClockDomain.h"
#include "core/transport/IUdpDatagramHandler.h"
#include "platform/windows/network/IocpUdpReceiver.h"

namespace airplaywin::windows::timing {

struct WindowsPtpTimingServiceOptions final {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t event_port{319U};
    std::uint16_t general_port{320U};
    std::size_t receive_depth{4U};
    bool join_multicast{true};
    std::string multicast_interface_address{"0.0.0.0"};
};

struct WindowsPtpTimingServiceDiagnostics final {
    bool running{false};
    std::uint16_t event_port{0U};
    std::uint16_t general_port{0U};
    std::uint64_t event_datagrams{0U};
    std::uint64_t general_datagrams{0U};
    std::uint64_t receive_errors{0U};
    std::uint64_t truncated_datagrams{0U};
    bool multicast_joined{false};
    std::string multicast_interface_address{};
    std::uint32_t last_error{0U};
    airplaywin::timing::PtpClockDomainDiagnostics clock{};
};

class WindowsPtpTimingService final {
public:
    explicit WindowsPtpTimingService(
        std::shared_ptr<airplaywin::timing::PtpClockDomain> clock_domain);
    ~WindowsPtpTimingService();

    WindowsPtpTimingService(const WindowsPtpTimingService&) = delete;
    WindowsPtpTimingService& operator=(const WindowsPtpTimingService&) = delete;

    [[nodiscard]] bool Start(const WindowsPtpTimingServiceOptions& options = {});
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] WindowsPtpTimingServiceDiagnostics Diagnostics() const noexcept;
    [[nodiscard]] std::shared_ptr<airplaywin::timing::PtpClockDomain> ClockDomain()
        const noexcept;

private:
    class Handler final : public airplaywin::transport::IUdpDatagramHandler {
    public:
        Handler(std::shared_ptr<airplaywin::timing::PtpClockDomain> clock_domain,
                std::atomic<std::uint64_t>& counter) noexcept;

        void OnDatagram(std::span<const std::byte> datagram,
                        const airplaywin::transport::DatagramEndpoint& source,
                        std::int64_t arrival_time_nanoseconds) noexcept override;

    private:
        std::shared_ptr<airplaywin::timing::PtpClockDomain> clock_domain_{};
        std::atomic<std::uint64_t>& counter_;
    };

    std::shared_ptr<airplaywin::timing::PtpClockDomain> clock_domain_{};
    std::atomic<std::uint64_t> event_datagrams_{0U};
    std::atomic<std::uint64_t> general_datagrams_{0U};
    Handler event_handler_;
    Handler general_handler_;
    airplaywin::windows::network::IocpUdpReceiver event_receiver_;
    airplaywin::windows::network::IocpUdpReceiver general_receiver_;
    std::atomic<bool> running_{false};
    std::atomic<bool> multicast_joined_{false};
    std::string multicast_interface_address_{};
    std::atomic<std::uint32_t> last_error_{0U};
};

}  // namespace airplaywin::windows::timing
