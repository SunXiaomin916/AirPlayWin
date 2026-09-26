#include "platform/windows/timing/WindowsPtpTimingService.h"

#include <Windows.h>

#include <stdexcept>
#include <utility>
#include <vector>

#include "platform/windows/timing/QpcClock.h"

namespace airplaywin::windows::timing {

WindowsPtpTimingService::WindowsPtpTimingService(
    std::shared_ptr<airplaywin::timing::PtpClockDomain> clock_domain)
    : clock_domain_(std::move(clock_domain)),
      event_handler_(clock_domain_, event_datagrams_),
      general_handler_(clock_domain_, general_datagrams_),
      event_receiver_(event_handler_),
      general_receiver_(general_handler_) {
    if (!clock_domain_) {
        throw std::invalid_argument("PTP timing service requires a clock domain");
    }
}

WindowsPtpTimingService::~WindowsPtpTimingService() {
    Stop();
}

bool WindowsPtpTimingService::Start(const WindowsPtpTimingServiceOptions& options) {
    if (running_.load(std::memory_order_acquire) || options.bind_address.empty() ||
        options.receive_depth == 0U || options.receive_depth > 32U ||
        (options.event_port != 0U && options.event_port == options.general_port) ||
        options.multicast_interface_address.empty()) {
        last_error_.store(ERROR_INVALID_PARAMETER, std::memory_order_release);
        return false;
    }
    clock_domain_->Reset();
    multicast_interface_address_ = options.multicast_interface_address;
    multicast_joined_.store(false, std::memory_order_release);
    event_datagrams_.store(0U, std::memory_order_release);
    general_datagrams_.store(0U, std::memory_order_release);
    const std::vector<std::string> multicast_groups = options.join_multicast
                                                          ? std::vector<std::string>{
                                                                "224.0.1.129",
                                                                "224.0.0.107"}
                                                          : std::vector<std::string>{};
    const airplaywin::windows::network::IocpUdpReceiverOptions event_options{
        .bind_address = options.bind_address,
        .port = options.event_port,
        .receive_depth = options.receive_depth,
        .max_datagram_bytes = 2U * 1'024U,
        .exclusive_address = true,
        .multicast_groups = multicast_groups,
        .multicast_interface_address = options.multicast_interface_address,
    };
    const auto general_options = airplaywin::windows::network::IocpUdpReceiverOptions{
        .bind_address = options.bind_address,
        .port = options.general_port,
        .receive_depth = options.receive_depth,
        .max_datagram_bytes = 2U * 1'024U,
        .exclusive_address = true,
        .multicast_groups = multicast_groups,
        .multicast_interface_address = options.multicast_interface_address,
    };
    if (!event_receiver_.Start(event_options)) {
        last_error_.store(event_receiver_.Diagnostics().last_error,
                          std::memory_order_release);
        return false;
    }
    if (!general_receiver_.Start(general_options)) {
        last_error_.store(general_receiver_.Diagnostics().last_error,
                          std::memory_order_release);
        event_receiver_.Stop();
        return false;
    }
    running_.store(true, std::memory_order_release);
    multicast_joined_.store(options.join_multicast, std::memory_order_release);
    last_error_.store(ERROR_SUCCESS, std::memory_order_release);
    return true;
}

void WindowsPtpTimingService::Stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    general_receiver_.Stop();
    event_receiver_.Stop();
    multicast_joined_.store(false, std::memory_order_release);
}

bool WindowsPtpTimingService::Running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

WindowsPtpTimingServiceDiagnostics WindowsPtpTimingService::Diagnostics() const noexcept {
    clock_domain_->Update(QpcClock::Now());
    const auto event = event_receiver_.Diagnostics();
    const auto general = general_receiver_.Diagnostics();
    const auto receiver_error = event.last_error != 0U ? event.last_error : general.last_error;
    return {
        .running = Running(),
        .event_port = event.bound_port,
        .general_port = general.bound_port,
        .event_datagrams = event_datagrams_.load(std::memory_order_relaxed),
        .general_datagrams = general_datagrams_.load(std::memory_order_relaxed),
        .receive_errors = event.receive_errors + general.receive_errors,
        .truncated_datagrams = event.truncated_datagrams + general.truncated_datagrams,
        .multicast_joined = multicast_joined_.load(std::memory_order_relaxed),
        .multicast_interface_address = multicast_interface_address_,
        .last_error = last_error_.load(std::memory_order_relaxed) != 0U
                          ? last_error_.load(std::memory_order_relaxed)
                          : receiver_error,
        .clock = clock_domain_->Diagnostics(),
    };
}

std::shared_ptr<airplaywin::timing::PtpClockDomain>
WindowsPtpTimingService::ClockDomain() const noexcept {
    return clock_domain_;
}

WindowsPtpTimingService::Handler::Handler(
    std::shared_ptr<airplaywin::timing::PtpClockDomain> clock_domain,
    std::atomic<std::uint64_t>& counter) noexcept
    : clock_domain_(std::move(clock_domain)), counter_(counter) {}

void WindowsPtpTimingService::Handler::OnDatagram(
    const std::span<const std::byte> datagram,
    const airplaywin::transport::DatagramEndpoint& source,
    const std::int64_t arrival_time_nanoseconds) noexcept {
    static_cast<void>(source);
    static_cast<void>(arrival_time_nanoseconds);
    counter_.fetch_add(1U, std::memory_order_relaxed);
    clock_domain_->OnDatagram(datagram, QpcClock::Now());
}

}  // namespace airplaywin::windows::timing
