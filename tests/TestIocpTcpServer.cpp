#include "TestFramework.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>

#include "core/crypto/OpenSessionAuthenticator.h"
#include "core/protocol/AirPlayControlService.h"
#include "platform/windows/network/IocpTcpServer.h"

namespace {

class ClientWinsock final {
public:
    ClientWinsock() { APW_EXPECT(WSAStartup(MAKEWORD(2, 2), &data_) == 0); }
    ~ClientWinsock() { static_cast<void>(WSACleanup()); }

private:
    WSADATA data_{};
};

[[nodiscard]] SOCKET Connect(const std::uint16_t port) {
    const SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    APW_EXPECT(socket_handle != INVALID_SOCKET);
    DWORD timeout = 2'000U;
    APW_EXPECT(setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    APW_EXPECT(InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    APW_EXPECT(connect(socket_handle, reinterpret_cast<const sockaddr*>(&address),
                       sizeof(address)) == 0);
    return socket_handle;
}

void SendAll(const SOCKET socket_handle, const std::string_view text) {
    std::size_t offset = 0U;
    while (offset < text.size()) {
        const int sent = send(socket_handle, text.data() + offset,
                              static_cast<int>(text.size() - offset), 0);
        APW_EXPECT(sent > 0);
        offset += static_cast<std::size_t>(sent);
    }
}

[[nodiscard]] std::string ReceiveHead(const SOCKET socket_handle) {
    std::string response;
    char buffer[256]{};
    while (response.find("\r\n\r\n") == std::string::npos) {
        const int received = recv(socket_handle, buffer, sizeof(buffer), 0);
        APW_EXPECT(received > 0);
        response.append(buffer, static_cast<std::size_t>(received));
    }
    return response;
}

template <typename Predicate>
[[nodiscard]] bool WaitUntil(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return predicate();
}

}  // namespace

void TestIocpTcpServer() {
    ClientWinsock client_winsock;
    airplaywin::crypto::OpenSessionAuthenticator authenticator;
    airplaywin::protocol::AirPlayControlService service{authenticator};
    airplaywin::windows::network::IocpTcpServer server{service};
    APW_EXPECT(server.Start({.bind_address = "127.0.0.1",
                             .port = 0U,
                             .idle_timeout = std::chrono::seconds{2}}));
    APW_EXPECT(server.BoundPort() != 0U);

    const SOCKET client = Connect(server.BoundPort());
    SendAll(client, "OPTI");
    SendAll(client,
            "ONS * RTSP/1.0\r\nCSeq: 9\r\nContent-Length: 0\r\n\r\n");
    const auto options_response = ReceiveHead(client);
    APW_EXPECT(options_response.find("RTSP/1.0 200 OK") != std::string::npos);
    APW_EXPECT(options_response.find("CSeq: 9") != std::string::npos);

    SendAll(client,
            "TEARDOWN * RTSP/1.0\r\nCSeq: 10\r\nContent-Length: 0\r\n\r\n");
    const auto teardown_response = ReceiveHead(client);
    APW_EXPECT(teardown_response.find("RTSP/1.0 200 OK") != std::string::npos);
    APW_EXPECT(teardown_response.find("Connection: close") != std::string::npos);
    static_cast<void>(closesocket(client));
    APW_EXPECT(WaitUntil([&server] { return server.Diagnostics().active_connections == 0U; },
                         std::chrono::seconds{2}));
    const auto diagnostics = server.Diagnostics();
    APW_EXPECT(diagnostics.accepted_connections == 1U);
    APW_EXPECT(diagnostics.received_bytes > 0U);
    APW_EXPECT(diagnostics.sent_bytes > 0U);
    APW_EXPECT(diagnostics.transport_errors == 0U);
    APW_EXPECT(diagnostics.handler_errors == 0U);
    server.Stop();
    APW_EXPECT(!server.Running());

    APW_EXPECT(server.Start({.bind_address = "127.0.0.1",
                             .port = 0U,
                             .idle_timeout = std::chrono::seconds{2}}));
    const SOCKET recovered_client = Connect(server.BoundPort());
    SendAll(recovered_client,
            "OPTIONS * RTSP/1.0\r\nCSeq: 11\r\nContent-Length: 0\r\n\r\n");
    APW_EXPECT(ReceiveHead(recovered_client).find("RTSP/1.0 200 OK") !=
               std::string::npos);
    static_cast<void>(closesocket(recovered_client));
    APW_EXPECT(WaitUntil([&server] { return server.Diagnostics().active_connections == 0U; },
                         std::chrono::seconds{2}));
    APW_EXPECT(server.Diagnostics().accepted_connections == 2U);
    APW_EXPECT(service.Diagnostics().peer_disconnects == 1U);
    server.Stop();

    airplaywin::protocol::AirPlayControlService timeout_service{authenticator};
    airplaywin::windows::network::IocpTcpServer timeout_server{timeout_service};
    APW_EXPECT(timeout_server.Start({.bind_address = "127.0.0.1",
                                     .port = 0U,
                                     .idle_timeout = std::chrono::milliseconds{300}}));
    const SOCKET idle_client = Connect(timeout_server.BoundPort());
    APW_EXPECT(WaitUntil(
        [&timeout_server] { return timeout_server.Diagnostics().idle_timeouts == 1U; },
        std::chrono::seconds{2}));
    static_cast<void>(closesocket(idle_client));
    timeout_server.Stop();
}
