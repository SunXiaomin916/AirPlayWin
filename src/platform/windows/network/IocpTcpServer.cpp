#include "platform/windows/network/IocpTcpServer.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace airplaywin::windows::network {

namespace {

constexpr ULONG_PTR kShutdownKey = std::numeric_limits<ULONG_PTR>::max();
constexpr DWORD kCompletionPollMilliseconds = 200U;

class WinsockLifetime final {
public:
    [[nodiscard]] bool Start() noexcept {
        if (started_) {
            return true;
        }
        WSADATA data{};
        started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        return started_;
    }

    void Stop() noexcept {
        if (started_) {
            static_cast<void>(WSACleanup());
            started_ = false;
        }
    }

    ~WinsockLifetime() { Stop(); }

private:
    bool started_{false};
};

}  // namespace

class IocpTcpServer::Impl final {
public:
    explicit Impl(transport::IControlConnectionHandler& handler) : handler_(handler) {}

    ~Impl() { Stop(); }

    [[nodiscard]] bool Start(const IocpTcpServerOptions& options) {
        if (running_.load(std::memory_order_acquire) || options.max_connections == 0U ||
            options.receive_chunk_bytes == 0U || options.receive_chunk_bytes > 64U * 1'024U ||
            options.max_pending_write_bytes == 0U ||
            options.idle_timeout <= std::chrono::milliseconds::zero()) {
            last_error_.store(ERROR_INVALID_PARAMETER, std::memory_order_release);
            return false;
        }
        options_ = options;
        if (!winsock_.Start()) {
            last_error_.store(static_cast<std::uint32_t>(WSAGetLastError()),
                              std::memory_order_release);
            return false;
        }
        completion_port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0U, 1U);
        if (completion_port_ == nullptr) {
            last_error_.store(GetLastError(), std::memory_order_release);
            winsock_.Stop();
            return false;
        }

        SOCKET listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0U,
                                     WSA_FLAG_OVERLAPPED);
        if (listener == INVALID_SOCKET) {
            FailStart(static_cast<std::uint32_t>(WSAGetLastError()));
            return false;
        }
        BOOL enabled = TRUE;
        if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR) {
            const auto error = static_cast<std::uint32_t>(WSAGetLastError());
            static_cast<void>(closesocket(listener));
            FailStart(error);
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(options.port);
        if (InetPtonA(AF_INET, options.bind_address.c_str(), &address.sin_addr) != 1 ||
            bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
                SOCKET_ERROR ||
            listen(listener, SOMAXCONN) == SOCKET_ERROR) {
            const auto error = static_cast<std::uint32_t>(WSAGetLastError());
            static_cast<void>(closesocket(listener));
            FailStart(error);
            return false;
        }
        u_long nonblocking = 1U;
        if (ioctlsocket(listener, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            const auto error = static_cast<std::uint32_t>(WSAGetLastError());
            static_cast<void>(closesocket(listener));
            FailStart(error);
            return false;
        }
        int address_length = sizeof(address);
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) ==
            SOCKET_ERROR) {
            const auto error = static_cast<std::uint32_t>(WSAGetLastError());
            static_cast<void>(closesocket(listener));
            FailStart(error);
            return false;
        }
        {
            std::scoped_lock lock{listener_mutex_};
            listener_ = listener;
        }
        bound_port_.store(ntohs(address.sin_port), std::memory_order_release);
        stop_requested_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        try {
            worker_thread_ = std::thread{[this] { WorkerLoop(); }};
            accept_thread_ = std::thread{[this] { AcceptLoop(); }};
        } catch (...) {
            running_.store(false, std::memory_order_release);
            stop_requested_.store(true, std::memory_order_release);
            CloseListener();
            if (worker_thread_.joinable()) {
                static_cast<void>(
                    PostQueuedCompletionStatus(completion_port_, 0U, kShutdownKey, nullptr));
                worker_thread_.join();
            }
            FailStart(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
        last_error_.store(ERROR_SUCCESS, std::memory_order_release);
        return true;
    }

    void Stop() noexcept {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        stop_requested_.store(true, std::memory_order_release);
        accept_wait_.notify_all();
        CloseListener();
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        if (completion_port_ != nullptr) {
            static_cast<void>(
                PostQueuedCompletionStatus(completion_port_, 0U, kShutdownKey, nullptr));
        }
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        if (completion_port_ != nullptr) {
            static_cast<void>(CloseHandle(completion_port_));
            completion_port_ = nullptr;
        }
        bound_port_.store(0U, std::memory_order_release);
        winsock_.Stop();
    }

    [[nodiscard]] bool Running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint16_t BoundPort() const noexcept {
        return bound_port_.load(std::memory_order_acquire);
    }

    [[nodiscard]] IocpTcpServerDiagnostics Diagnostics() const noexcept {
        return IocpTcpServerDiagnostics{
            .running = Running(),
            .bound_port = BoundPort(),
            .accepted_connections = accepted_connections_.load(std::memory_order_relaxed),
            .active_connections = active_connections_.load(std::memory_order_relaxed),
            .received_bytes = received_bytes_.load(std::memory_order_relaxed),
            .sent_bytes = sent_bytes_.load(std::memory_order_relaxed),
            .idle_timeouts = idle_timeouts_.load(std::memory_order_relaxed),
            .rejected_connections = rejected_connections_.load(std::memory_order_relaxed),
            .transport_errors = transport_errors_.load(std::memory_order_relaxed),
            .handler_errors = handler_errors_.load(std::memory_order_relaxed),
            .last_error = last_error_.load(std::memory_order_relaxed),
        };
    }

private:
    struct Connection final {
        SOCKET socket{INVALID_SOCKET};
        transport::ConnectionId id{0U};
        std::string peer_address{};
        std::string local_address{};
        std::chrono::steady_clock::time_point last_activity{std::chrono::steady_clock::now()};
        std::deque<std::vector<std::byte>> pending_writes{};
        std::size_t send_offset{0U};
        std::size_t pending_write_bytes{0U};
        std::size_t outstanding_operations{0U};
        bool send_in_progress{false};
        bool close_after_writes{false};
        bool closed{false};
        bool handler_notified{false};
    };

    enum class OperationKind : std::uint8_t {
        Accepted,
        Receive,
        Send,
    };

    struct Operation final {
        OVERLAPPED overlapped{};
        OperationKind kind{OperationKind::Receive};
        std::shared_ptr<Connection> connection{};
        std::vector<std::byte> receive_storage{};
    };

    void FailStart(const std::uint32_t error) noexcept {
        last_error_.store(error, std::memory_order_release);
        if (completion_port_ != nullptr) {
            static_cast<void>(CloseHandle(completion_port_));
            completion_port_ = nullptr;
        }
        winsock_.Stop();
    }

    void CloseListener() noexcept {
        std::scoped_lock lock{listener_mutex_};
        if (listener_ != INVALID_SOCKET) {
            static_cast<void>(closesocket(listener_));
            listener_ = INVALID_SOCKET;
        }
    }

    void AcceptLoop() noexcept {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            SOCKET listener = INVALID_SOCKET;
            {
                std::scoped_lock lock{listener_mutex_};
                listener = listener_;
            }
            if (listener == INVALID_SOCKET) {
                break;
            }
            sockaddr_in peer{};
            int peer_length = sizeof(peer);
            const SOCKET accepted =
                accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
            if (accepted == INVALID_SOCKET) {
                const auto error = static_cast<std::uint32_t>(WSAGetLastError());
                if (error == WSAEWOULDBLOCK &&
                    !stop_requested_.load(std::memory_order_acquire)) {
                    std::unique_lock wait_lock{accept_wait_mutex_};
                    accept_wait_.wait_for(wait_lock, std::chrono::milliseconds{50}, [this] {
                        return stop_requested_.load(std::memory_order_acquire);
                    });
                    continue;
                }
                if (!stop_requested_.load(std::memory_order_acquire)) {
                    transport_errors_.fetch_add(1U, std::memory_order_relaxed);
                    last_error_.store(error, std::memory_order_relaxed);
                }
                break;
            }
            u_long blocking = 0U;
            static_cast<void>(ioctlsocket(accepted, FIONBIO, &blocking));
            auto connection = std::make_shared<Connection>();
            connection->socket = accepted;
            const auto serial = next_connection_.fetch_add(1U, std::memory_order_relaxed);
            connection->id = (static_cast<std::uint64_t>(BoundPort()) << 48U) | serial;
            char peer_text[INET_ADDRSTRLEN]{};
            if (InetNtopA(AF_INET, &peer.sin_addr, peer_text, sizeof(peer_text)) != nullptr) {
                connection->peer_address = peer_text;
            }
            sockaddr_in local{};
            int local_length = sizeof(local);
            if (getsockname(accepted, reinterpret_cast<sockaddr*>(&local), &local_length) !=
                SOCKET_ERROR) {
                char local_text[INET_ADDRSTRLEN]{};
                if (InetNtopA(AF_INET, &local.sin_addr, local_text, sizeof(local_text)) !=
                    nullptr) {
                    connection->local_address = local_text;
                }
            }
            auto operation = std::make_unique<Operation>();
            operation->kind = OperationKind::Accepted;
            operation->connection = std::move(connection);
            if (!PostQueuedCompletionStatus(completion_port_, 0U, 0U,
                                            &operation->overlapped)) {
                const auto error = GetLastError();
                static_cast<void>(closesocket(accepted));
                transport_errors_.fetch_add(1U, std::memory_order_relaxed);
                last_error_.store(error, std::memory_order_relaxed);
                continue;
            }
            static_cast<void>(operation.release());
        }
    }

    void WorkerLoop() noexcept {
        bool shutting_down = false;
        for (;;) {
            DWORD bytes = 0U;
            ULONG_PTR completion_key = 0U;
            OVERLAPPED* overlapped = nullptr;
            const BOOL succeeded = GetQueuedCompletionStatus(completion_port_, &bytes,
                                                              &completion_key, &overlapped,
                                                              kCompletionPollMilliseconds);
            const auto error = succeeded ? ERROR_SUCCESS : GetLastError();
            if (overlapped == nullptr) {
                if (completion_key == kShutdownKey) {
                    shutting_down = true;
                    ShutdownConnections();
                } else if (!succeeded && error != WAIT_TIMEOUT) {
                    transport_errors_.fetch_add(1U, std::memory_order_relaxed);
                    last_error_.store(error, std::memory_order_relaxed);
                }
                ScanIdleConnections();
                if (shutting_down && outstanding_operations_ == 0U) {
                    connections_.clear();
                    break;
                }
                continue;
            }

            std::unique_ptr<Operation> operation{reinterpret_cast<Operation*>(overlapped)};
            const auto connection = operation->connection;
            if (operation->kind != OperationKind::Accepted) {
                --outstanding_operations_;
                if (connection->outstanding_operations != 0U) {
                    --connection->outstanding_operations;
                }
            }
            if (operation->kind == OperationKind::Accepted) {
                HandleAccepted(connection, shutting_down);
            } else if (operation->kind == OperationKind::Receive) {
                HandleReceive(connection, succeeded != FALSE, error, bytes,
                              operation->receive_storage);
            } else {
                HandleSend(connection, succeeded != FALSE, error, bytes);
            }
            MaybeErase(connection);
            ScanIdleConnections();
            if (shutting_down && outstanding_operations_ == 0U) {
                connections_.clear();
                break;
            }
        }
    }

    void HandleAccepted(const std::shared_ptr<Connection>& connection,
                        const bool shutting_down) noexcept {
        if (shutting_down || stop_requested_.load(std::memory_order_acquire) ||
            connections_.size() >= options_.max_connections) {
            rejected_connections_.fetch_add(1U, std::memory_order_relaxed);
            static_cast<void>(closesocket(connection->socket));
            connection->socket = INVALID_SOCKET;
            connection->closed = true;
            return;
        }
        BOOL enabled = TRUE;
        static_cast<void>(setsockopt(connection->socket, IPPROTO_TCP, TCP_NODELAY,
                                     reinterpret_cast<const char*>(&enabled), sizeof(enabled)));
        static_cast<void>(setsockopt(connection->socket, SOL_SOCKET, SO_KEEPALIVE,
                                     reinterpret_cast<const char*>(&enabled), sizeof(enabled)));
        if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(connection->socket), completion_port_,
                                   static_cast<ULONG_PTR>(connection->id), 0U) == nullptr) {
            const auto error = GetLastError();
            static_cast<void>(closesocket(connection->socket));
            connection->socket = INVALID_SOCKET;
            connection->closed = true;
            transport_errors_.fetch_add(1U, std::memory_order_relaxed);
            last_error_.store(error, std::memory_order_relaxed);
            return;
        }
        connections_.emplace(connection->id, connection);
        accepted_connections_.fetch_add(1U, std::memory_order_relaxed);
        active_connections_.fetch_add(1U, std::memory_order_relaxed);
        try {
            connection->handler_notified = true;
            handler_.OnConnected(connection->id, connection->peer_address,
                                 connection->local_address);
        } catch (...) {
            handler_errors_.fetch_add(1U, std::memory_order_relaxed);
            CloseConnection(connection, transport::DisconnectReason::ProtocolError);
            return;
        }
        PostReceive(connection);
    }

    void PostReceive(const std::shared_ptr<Connection>& connection) noexcept {
        if (connection->closed || connection->close_after_writes) {
            return;
        }
        auto operation = std::make_unique<Operation>();
        operation->kind = OperationKind::Receive;
        operation->connection = connection;
        operation->receive_storage.resize(options_.receive_chunk_bytes);
        WSABUF buffer{
            .len = static_cast<ULONG>(operation->receive_storage.size()),
            .buf = reinterpret_cast<char*>(operation->receive_storage.data()),
        };
        DWORD flags = 0U;
        DWORD received = 0U;
        const int result = WSARecv(connection->socket, &buffer, 1U, &received, &flags,
                                   &operation->overlapped, nullptr);
        if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            const auto error = static_cast<std::uint32_t>(WSAGetLastError());
            transport_errors_.fetch_add(1U, std::memory_order_relaxed);
            last_error_.store(error, std::memory_order_relaxed);
            CloseConnection(connection, transport::DisconnectReason::TransportError);
            return;
        }
        ++outstanding_operations_;
        ++connection->outstanding_operations;
        static_cast<void>(operation.release());
    }

    void HandleReceive(const std::shared_ptr<Connection>& connection,
                       const bool succeeded,
                       const std::uint32_t error,
                       const DWORD bytes,
                       const std::vector<std::byte>& storage) noexcept {
        if (connection->closed) {
            return;
        }
        if (!succeeded || bytes == 0U) {
            if (!succeeded && error != ERROR_OPERATION_ABORTED) {
                transport_errors_.fetch_add(1U, std::memory_order_relaxed);
                last_error_.store(error, std::memory_order_relaxed);
            }
            CloseConnection(connection, bytes == 0U ? transport::DisconnectReason::PeerClosed
                                                     : transport::DisconnectReason::TransportError);
            return;
        }
        received_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        connection->last_activity = std::chrono::steady_clock::now();
        try {
            auto reply = handler_.OnBytes(connection->id,
                                          std::span<const std::byte>{storage.data(), bytes});
            for (auto& write : reply.writes) {
                if (write.size() > options_.max_pending_write_bytes -
                                       std::min(connection->pending_write_bytes,
                                                options_.max_pending_write_bytes)) {
                    CloseConnection(connection, transport::DisconnectReason::ProtocolError);
                    return;
                }
                connection->pending_write_bytes += write.size();
                connection->pending_writes.push_back(std::move(write));
            }
            connection->close_after_writes = reply.close_after_writes;
        } catch (...) {
            handler_errors_.fetch_add(1U, std::memory_order_relaxed);
            CloseConnection(connection, transport::DisconnectReason::ProtocolError);
            return;
        }
        StartSend(connection);
        if (connection->close_after_writes && connection->pending_writes.empty() &&
            !connection->send_in_progress) {
            CloseConnection(connection, transport::DisconnectReason::Requested);
        } else if (!connection->close_after_writes) {
            PostReceive(connection);
        }
    }

    void StartSend(const std::shared_ptr<Connection>& connection) noexcept {
        if (connection->closed || connection->send_in_progress ||
            connection->pending_writes.empty()) {
            return;
        }
        const auto& front = connection->pending_writes.front();
        const auto remaining = front.size() - connection->send_offset;
        auto operation = std::make_unique<Operation>();
        operation->kind = OperationKind::Send;
        operation->connection = connection;
        WSABUF buffer{
            .len = static_cast<ULONG>(remaining),
            .buf = reinterpret_cast<char*>(const_cast<std::byte*>(front.data() +
                                                                  connection->send_offset)),
        };
        DWORD sent = 0U;
        const int result = WSASend(connection->socket, &buffer, 1U, &sent, 0U,
                                   &operation->overlapped, nullptr);
        if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            const auto error = static_cast<std::uint32_t>(WSAGetLastError());
            transport_errors_.fetch_add(1U, std::memory_order_relaxed);
            last_error_.store(error, std::memory_order_relaxed);
            CloseConnection(connection, transport::DisconnectReason::TransportError);
            return;
        }
        connection->send_in_progress = true;
        ++outstanding_operations_;
        ++connection->outstanding_operations;
        static_cast<void>(operation.release());
    }

    void HandleSend(const std::shared_ptr<Connection>& connection,
                    const bool succeeded,
                    const std::uint32_t error,
                    const DWORD bytes) noexcept {
        connection->send_in_progress = false;
        if (connection->closed) {
            return;
        }
        if (!succeeded || bytes == 0U || connection->pending_writes.empty()) {
            if (!succeeded && error != ERROR_OPERATION_ABORTED) {
                transport_errors_.fetch_add(1U, std::memory_order_relaxed);
                last_error_.store(error, std::memory_order_relaxed);
            }
            CloseConnection(connection, transport::DisconnectReason::TransportError);
            return;
        }
        sent_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        connection->send_offset += bytes;
        if (connection->send_offset >= connection->pending_writes.front().size()) {
            connection->pending_write_bytes -= connection->pending_writes.front().size();
            connection->pending_writes.pop_front();
            connection->send_offset = 0U;
        }
        if (!connection->pending_writes.empty()) {
            StartSend(connection);
        } else if (connection->close_after_writes) {
            CloseConnection(connection, transport::DisconnectReason::Requested);
        }
    }

    void CloseConnection(const std::shared_ptr<Connection>& connection,
                         const transport::DisconnectReason reason) noexcept {
        if (connection->closed) {
            return;
        }
        connection->closed = true;
        if (connection->socket != INVALID_SOCKET) {
            static_cast<void>(shutdown(connection->socket, SD_BOTH));
            static_cast<void>(closesocket(connection->socket));
            connection->socket = INVALID_SOCKET;
        }
        if (connection->handler_notified) {
            try {
                handler_.OnDisconnected(connection->id, reason);
            } catch (...) {
                handler_errors_.fetch_add(1U, std::memory_order_relaxed);
            }
            connection->handler_notified = false;
        }
        active_connections_.fetch_sub(1U, std::memory_order_relaxed);
    }

    void MaybeErase(const std::shared_ptr<Connection>& connection) noexcept {
        if (connection->closed && connection->outstanding_operations == 0U) {
            connections_.erase(connection->id);
        }
    }

    void ScanIdleConnections() noexcept {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<Connection>> expired;
        for (const auto& [id, connection] : connections_) {
            static_cast<void>(id);
            if (!connection->closed && now - connection->last_activity >= options_.idle_timeout) {
                expired.push_back(connection);
            }
        }
        for (const auto& connection : expired) {
            idle_timeouts_.fetch_add(1U, std::memory_order_relaxed);
            CloseConnection(connection, transport::DisconnectReason::IdleTimeout);
            MaybeErase(connection);
        }
    }

    void ShutdownConnections() noexcept {
        std::vector<std::shared_ptr<Connection>> connections;
        connections.reserve(connections_.size());
        for (const auto& [id, connection] : connections_) {
            static_cast<void>(id);
            connections.push_back(connection);
        }
        for (const auto& connection : connections) {
            CloseConnection(connection, transport::DisconnectReason::ServerShutdown);
            MaybeErase(connection);
        }
    }

    transport::IControlConnectionHandler& handler_;
    IocpTcpServerOptions options_{};
    WinsockLifetime winsock_{};
    mutable std::mutex listener_mutex_{};
    std::mutex accept_wait_mutex_{};
    std::condition_variable accept_wait_{};
    SOCKET listener_{INVALID_SOCKET};
    HANDLE completion_port_{nullptr};
    std::thread accept_thread_{};
    std::thread worker_thread_{};
    std::unordered_map<transport::ConnectionId, std::shared_ptr<Connection>> connections_{};
    std::size_t outstanding_operations_{0U};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint16_t> bound_port_{0U};
    std::atomic<std::uint64_t> next_connection_{1U};
    std::atomic<std::uint64_t> accepted_connections_{0U};
    std::atomic<std::uint64_t> active_connections_{0U};
    std::atomic<std::uint64_t> received_bytes_{0U};
    std::atomic<std::uint64_t> sent_bytes_{0U};
    std::atomic<std::uint64_t> idle_timeouts_{0U};
    std::atomic<std::uint64_t> rejected_connections_{0U};
    std::atomic<std::uint64_t> transport_errors_{0U};
    std::atomic<std::uint64_t> handler_errors_{0U};
    std::atomic<std::uint32_t> last_error_{0U};
};

IocpTcpServer::IocpTcpServer(transport::IControlConnectionHandler& handler)
    : impl_(std::make_unique<Impl>(handler)) {}

IocpTcpServer::~IocpTcpServer() = default;

bool IocpTcpServer::Start(const IocpTcpServerOptions& options) {
    return impl_->Start(options);
}

void IocpTcpServer::Stop() noexcept {
    impl_->Stop();
}

bool IocpTcpServer::Running() const noexcept {
    return impl_->Running();
}

std::uint16_t IocpTcpServer::BoundPort() const noexcept {
    return impl_->BoundPort();
}

IocpTcpServerDiagnostics IocpTcpServer::Diagnostics() const noexcept {
    return impl_->Diagnostics();
}

}  // namespace airplaywin::windows::network
