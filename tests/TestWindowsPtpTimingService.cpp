#include "TestFramework.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "core/timing/PtpClockDomain.h"
#include "platform/windows/timing/QpcClock.h"
#include "platform/windows/timing/WindowsPtpTimingService.h"

namespace {

[[nodiscard]] bool SendDatagram(const std::uint16_t port,
                                const std::span<const std::byte> bytes) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
    const SOCKET socket_value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_value == INVALID_SOCKET) {
        static_cast<void>(WSACleanup());
        return false;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    static_cast<void>(InetPtonA(AF_INET, "127.0.0.1", &destination.sin_addr));
    const auto sent = sendto(socket_value, reinterpret_cast<const char*>(bytes.data()),
                             static_cast<int>(bytes.size()), 0,
                             reinterpret_cast<const sockaddr*>(&destination),
                             sizeof(destination));
    static_cast<void>(closesocket(socket_value));
    static_cast<void>(WSACleanup());
    return sent == static_cast<int>(bytes.size());
}

}  // namespace

void TestWindowsPtpTimingService() {
    using airplaywin::timing::ClockServoConfig;
    using airplaywin::timing::PtpClockDomain;
    using airplaywin::windows::timing::QpcClock;
    using airplaywin::windows::timing::WindowsPtpTimingService;

    auto domain = std::make_shared<PtpClockDomain>(ClockServoConfig{
        .local_clock_frequency = QpcClock::Frequency(),
    });
    WindowsPtpTimingService service{domain};
    APW_EXPECT(service.Start({.bind_address = "127.0.0.1",
                              .event_port = 0U,
                              .general_port = 0U,
                              .receive_depth = 2U,
                              .join_multicast = false}));
    const auto started = service.Diagnostics();
    APW_EXPECT(started.running);
    APW_EXPECT(started.event_port != 0U);
    APW_EXPECT(started.general_port != 0U);
    APW_EXPECT(started.event_port != started.general_port);

    auto conflicting_domain = std::make_shared<PtpClockDomain>(ClockServoConfig{
        .local_clock_frequency = QpcClock::Frequency(),
    });
    WindowsPtpTimingService conflicting_service{conflicting_domain};
    APW_EXPECT(!conflicting_service.Start({.bind_address = "127.0.0.1",
                                           .event_port = started.event_port,
                                           .general_port = 0U,
                                           .receive_depth = 1U,
                                           .join_multicast = false}));

    const std::array invalid{std::byte{1U}, std::byte{2U}, std::byte{3U}};
    APW_EXPECT(SendDatagram(started.event_port, invalid));
    APW_EXPECT(SendDatagram(started.general_port, invalid));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    auto diagnostics = service.Diagnostics();
    while ((diagnostics.event_datagrams < 1U || diagnostics.general_datagrams < 1U) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        diagnostics = service.Diagnostics();
    }
    APW_EXPECT(diagnostics.event_datagrams == 1U);
    APW_EXPECT(diagnostics.general_datagrams == 1U);
    APW_EXPECT(diagnostics.clock.received_datagrams == 2U);
    APW_EXPECT(diagnostics.clock.invalid_datagrams == 2U);
    APW_EXPECT(diagnostics.receive_errors == 0U);
    service.Stop();
    APW_EXPECT(!service.Diagnostics().running);

    APW_EXPECT(!service.Start({.bind_address = "127.0.0.1",
                               .event_port = 40000U,
                               .general_port = 40000U}));
}
