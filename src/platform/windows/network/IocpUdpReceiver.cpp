#include "platform/windows/network/IocpUdpReceiver.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace airplaywin::windows::network {

namespace {

constexpr ULONG_PTR kShutdownKey = std::numeric_limits<ULONG_PTR>::max();

[[nodiscard]] std::int64_t SteadyNanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

class IocpUdpReceiver::Impl final {
public:
    explicit Impl(transport::IUdpDatagramHandler& handler) : handler_(handler) {}

    ~Impl() { Stop(); }

    [[nodiscard]] bool Start(const IocpUdpReceiverOptions& options) {
        if (running_.load(std::memory_order_acquire) || options.receive_depth == 0U ||
            options.receive_depth > 32U || options.max_datagram_bytes < 12U ||
            options.max_datagram_bytes > 65'507U) {
            last_error_.store(ERROR_INVALID_PARAMETER, std::memory_order_release);
            return false;
        }
        options_ = options;
        WSADATA data{};
        const int startup_result = WSAStartup(MAKEWORD(2, 2), &data);
        if (startup_result != 0) {
            last_error_.store(static_cast<std::uint32_t>(startup_result),
                              std::memory_order_release);
            return false;
        }
        winsock_started_ = true;
        socket_ = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0U,
                             WSA_FLAG_OVERLAPPED);
        if (socket_ == INVALID_SOCKET) {
            FailStart(static_cast<std::uint32_t>(WSAGetLastError()));
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(options.port);
        if (InetPtonA(AF_INET, options.bind_address.c_str(), &address.sin_addr) != 1 ||
            bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
                SOCKET_ERROR) {
            FailStart(static_cast<std::uint32_t>(WSAGetLastError()));
            return false;
        }
        int address_length = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &address_length) ==
            SOCKET_ERROR) {
            FailStart(static_cast<std::uint32_t>(WSAGetLastError()));
            return false;
        }
        completion_port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0U, 1U);
        if (completion_port_ == nullptr ||
            CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket_), completion_port_, 1U, 0U) ==
                nullptr) {
            FailStart(GetLastError());
            return false;
        }
        bound_port_.store(ntohs(address.sin_port), std::memory_order_release);
        operations_.reserve(options.receive_depth);
        for (std::size_t index = 0U; index < options.receive_depth; ++index) {
            auto operation = std::make_unique<Operation>();
            operation->storage.resize(options.max_datagram_bytes);
            operations_.push_back(std::move(operation));
        }
        running_.store(true, std::memory_order_release);
        try {
            worker_ = std::thread{[this] { WorkerLoop(); }};
        } catch (...) {
            running_.store(false, std::memory_order_release);
            FailStart(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
        for (auto& operation : operations_) {
            if (!PostReceive(*operation)) {
                Stop();
                return false;
            }
        }
        last_error_.store(ERROR_SUCCESS, std::memory_order_release);
        return true;
    }

    void Stop() noexcept {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (socket_ != INVALID_SOCKET) {
            static_cast<void>(closesocket(socket_));
            socket_ = INVALID_SOCKET;
        }
        if (completion_port_ != nullptr) {
            static_cast<void>(
                PostQueuedCompletionStatus(completion_port_, 0U, kShutdownKey, nullptr));
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        operations_.clear();
        if (completion_port_ != nullptr) {
            static_cast<void>(CloseHandle(completion_port_));
            completion_port_ = nullptr;
        }
        bound_port_.store(0U, std::memory_order_release);
        if (winsock_started_) {
            static_cast<void>(WSACleanup());
            winsock_started_ = false;
        }
    }

    [[nodiscard]] bool Running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint16_t BoundPort() const noexcept {
        return bound_port_.load(std::memory_order_acquire);
    }

    [[nodiscard]] IocpUdpReceiverDiagnostics Diagnostics() const noexcept {
        return IocpUdpReceiverDiagnostics{
            .running = Running(),
            .bound_port = BoundPort(),
            .received_datagrams = received_datagrams_.load(std::memory_order_relaxed),
            .received_bytes = received_bytes_.load(std::memory_order_relaxed),
            .truncated_datagrams = truncated_datagrams_.load(std::memory_order_relaxed),
            .receive_errors = receive_errors_.load(std::memory_order_relaxed),
            .last_error = last_error_.load(std::memory_order_relaxed),
        };
    }

private:
    struct Operation final {
        OVERLAPPED overlapped{};
        sockaddr_in source{};
        int source_length{sizeof(sockaddr_in)};
        std::vector<std::byte> storage{};
    };

    [[nodiscard]] bool PostReceive(Operation& operation) noexcept {
        operation.overlapped = {};
        operation.source = {};
        operation.source_length = sizeof(operation.source);
        WSABUF buffer{
            .len = static_cast<ULONG>(operation.storage.size()),
            .buf = reinterpret_cast<char*>(operation.storage.data()),
        };
        DWORD flags = 0U;
        DWORD received = 0U;
        const int result = WSARecvFrom(socket_, &buffer, 1U, &received, &flags,
                                       reinterpret_cast<sockaddr*>(&operation.source),
                                       &operation.source_length, &operation.overlapped, nullptr);
        if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            const auto error = static_cast<std::uint32_t>(WSAGetLastError());
            receive_errors_.fetch_add(1U, std::memory_order_relaxed);
            last_error_.store(error, std::memory_order_relaxed);
            return false;
        }
        outstanding_.fetch_add(1U, std::memory_order_acq_rel);
        return true;
    }

    void WorkerLoop() noexcept {
        bool shutting_down = false;
        for (;;) {
            DWORD bytes = 0U;
            ULONG_PTR key = 0U;
            OVERLAPPED* overlapped = nullptr;
            const BOOL succeeded =
                GetQueuedCompletionStatus(completion_port_, &bytes, &key, &overlapped, INFINITE);
            const auto error = succeeded ? ERROR_SUCCESS : GetLastError();
            if (overlapped == nullptr) {
                if (key == kShutdownKey) {
                    shutting_down = true;
                } else if (!succeeded) {
                    receive_errors_.fetch_add(1U, std::memory_order_relaxed);
                    last_error_.store(error, std::memory_order_relaxed);
                }
            } else {
                auto* const operation = reinterpret_cast<Operation*>(overlapped);
                outstanding_.fetch_sub(1U, std::memory_order_acq_rel);
                if (succeeded && bytes != 0U && running_.load(std::memory_order_acquire)) {
                    received_datagrams_.fetch_add(1U, std::memory_order_relaxed);
                    received_bytes_.fetch_add(bytes, std::memory_order_relaxed);
                    handler_.OnDatagram(
                        std::span<const std::byte>{operation->storage.data(), bytes},
                        transport::DatagramEndpoint{
                            .ipv4_address_network_order = operation->source.sin_addr.s_addr,
                            .port = ntohs(operation->source.sin_port),
                        },
                        SteadyNanoseconds());
                } else if (!succeeded && error == WSAEMSGSIZE) {
                    truncated_datagrams_.fetch_add(1U, std::memory_order_relaxed);
                } else if (!succeeded && error != ERROR_OPERATION_ABORTED) {
                    receive_errors_.fetch_add(1U, std::memory_order_relaxed);
                    last_error_.store(error, std::memory_order_relaxed);
                }
                if (running_.load(std::memory_order_acquire)) {
                    static_cast<void>(PostReceive(*operation));
                }
            }
            if (shutting_down && outstanding_.load(std::memory_order_acquire) == 0U) {
                break;
            }
        }
    }

    void FailStart(const std::uint32_t error) noexcept {
        last_error_.store(error, std::memory_order_release);
        if (socket_ != INVALID_SOCKET) {
            static_cast<void>(closesocket(socket_));
            socket_ = INVALID_SOCKET;
        }
        if (completion_port_ != nullptr) {
            static_cast<void>(CloseHandle(completion_port_));
            completion_port_ = nullptr;
        }
        if (winsock_started_) {
            static_cast<void>(WSACleanup());
            winsock_started_ = false;
        }
    }

    transport::IUdpDatagramHandler& handler_;
    IocpUdpReceiverOptions options_{};
    SOCKET socket_{INVALID_SOCKET};
    HANDLE completion_port_{nullptr};
    bool winsock_started_{false};
    std::vector<std::unique_ptr<Operation>> operations_{};
    std::thread worker_{};
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> outstanding_{0U};
    std::atomic<std::uint16_t> bound_port_{0U};
    std::atomic<std::uint64_t> received_datagrams_{0U};
    std::atomic<std::uint64_t> received_bytes_{0U};
    std::atomic<std::uint64_t> truncated_datagrams_{0U};
    std::atomic<std::uint64_t> receive_errors_{0U};
    std::atomic<std::uint32_t> last_error_{0U};
};

IocpUdpReceiver::IocpUdpReceiver(transport::IUdpDatagramHandler& handler)
    : impl_(std::make_unique<Impl>(handler)) {}

IocpUdpReceiver::~IocpUdpReceiver() = default;

bool IocpUdpReceiver::Start(const IocpUdpReceiverOptions& options) {
    return impl_->Start(options);
}

void IocpUdpReceiver::Stop() noexcept {
    impl_->Stop();
}

bool IocpUdpReceiver::Running() const noexcept {
    return impl_->Running();
}

std::uint16_t IocpUdpReceiver::BoundPort() const noexcept {
    return impl_->BoundPort();
}

IocpUdpReceiverDiagnostics IocpUdpReceiver::Diagnostics() const noexcept {
    return impl_->Diagnostics();
}

}  // namespace airplaywin::windows::network
