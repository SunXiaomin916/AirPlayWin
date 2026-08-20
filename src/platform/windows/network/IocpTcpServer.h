#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/transport/IControlConnectionHandler.h"

namespace airplaywin::windows::network {

struct IocpTcpServerOptions final {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{0U};
    std::size_t max_connections{32U};
    std::size_t receive_chunk_bytes{16U * 1'024U};
    std::size_t max_pending_write_bytes{2U * 1'024U * 1'024U};
    std::chrono::milliseconds idle_timeout{std::chrono::seconds{30}};
};

struct IocpTcpServerDiagnostics final {
    bool running{false};
    std::uint16_t bound_port{0U};
    std::uint64_t accepted_connections{0U};
    std::uint64_t active_connections{0U};
    std::uint64_t received_bytes{0U};
    std::uint64_t sent_bytes{0U};
    std::uint64_t idle_timeouts{0U};
    std::uint64_t rejected_connections{0U};
    std::uint64_t transport_errors{0U};
    std::uint64_t handler_errors{0U};
    std::uint32_t last_error{0U};
};

class IocpTcpServer final {
public:
    explicit IocpTcpServer(transport::IControlConnectionHandler& handler);
    ~IocpTcpServer();

    IocpTcpServer(const IocpTcpServer&) = delete;
    IocpTcpServer& operator=(const IocpTcpServer&) = delete;

    [[nodiscard]] bool Start(const IocpTcpServerOptions& options);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::uint16_t BoundPort() const noexcept;
    [[nodiscard]] IocpTcpServerDiagnostics Diagnostics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace airplaywin::windows::network
