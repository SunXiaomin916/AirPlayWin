#include "TestFramework.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>
#include <thread>

#include "core/transport/IUdpDatagramHandler.h"
#include "platform/windows/network/IocpUdpReceiver.h"

namespace {

class DatagramHandler final : public airplaywin::transport::IUdpDatagramHandler {
public:
    void OnDatagram(const std::span<const std::byte> datagram,
                    const airplaywin::transport::DatagramEndpoint& source,
                    const std::int64_t arrival_time_nanoseconds) noexcept override {
        bytes_.store(datagram.size(), std::memory_order_relaxed);
        source_port_.store(source.port, std::memory_order_relaxed);
        arrival_ns_.store(arrival_time_nanoseconds, std::memory_order_relaxed);
        count_.fetch_add(1U, std::memory_order_release);
    }

    [[nodiscard]] std::uint64_t Count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t Bytes() const noexcept {
        return bytes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint16_t SourcePort() const noexcept {
        return source_port_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::int64_t ArrivalNanoseconds() const noexcept {
        return arrival_ns_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> count_{0U};
    std::atomic<std::size_t> bytes_{0U};
    std::atomic<std::uint16_t> source_port_{0U};
    std::atomic<std::int64_t> arrival_ns_{0};
};

}  // namespace

void TestIocpUdpReceiver() {
    DatagramHandler handler;
    airplaywin::windows::network::IocpUdpReceiver receiver{handler};
    APW_EXPECT(receiver.Start({.bind_address = "127.0.0.1",
                               .port = 0U,
                               .receive_depth = 2U,
                               .max_datagram_bytes = 1'024U}));
    APW_EXPECT(receiver.Running());
    APW_EXPECT(receiver.BoundPort() != 0U);

    const SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    APW_EXPECT(sender != INVALID_SOCKET);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(receiver.BoundPort());
    APW_EXPECT(InetPtonA(AF_INET, "127.0.0.1", &destination.sin_addr) == 1);
    constexpr std::array<char, 5U> payload{'R', 'T', 'P', '!', '\0'};
    const int sent = sendto(sender, payload.data(), static_cast<int>(payload.size()), 0,
                            reinterpret_cast<const sockaddr*>(&destination),
                            sizeof(destination));
    APW_EXPECT(sent == static_cast<int>(payload.size()));
    static_cast<void>(closesocket(sender));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (handler.Count() == 0U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    APW_EXPECT(handler.Count() == 1U);
    APW_EXPECT(handler.Bytes() == payload.size());
    APW_EXPECT(handler.SourcePort() != 0U);
    APW_EXPECT(handler.ArrivalNanoseconds() > 0);
    const auto diagnostics = receiver.Diagnostics();
    APW_EXPECT(diagnostics.received_datagrams == 1U);
    APW_EXPECT(diagnostics.received_bytes == payload.size());
    APW_EXPECT(diagnostics.receive_errors == 0U);
    receiver.Stop();
    APW_EXPECT(!receiver.Running());
    APW_EXPECT(!receiver.Start({.bind_address = "127.0.0.1",
                                .port = 0U,
                                .receive_depth = 1U,
                                .max_datagram_bytes = 1'024U,
                                .multicast_groups = {"not-a-multicast-address"}}));
}
