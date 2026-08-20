#include <WinSock2.h>
#include <Windows.h>
#include <Ws2ipdef.h>
#include <Iphlpapi.h>
#include <windns.h>

#include "platform/windows/network/WindowsDiscoveryService.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/discovery/AirPlayServiceRecords.h"
#include "platform/windows/network/WindowsNetworkInterfaceEnumerator.h"

namespace airplaywin::windows::network {
namespace {

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        Reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }
    [[nodiscard]] bool IsValid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    void Reset() noexcept {
        if (IsValid()) {
            static_cast<void>(CloseHandle(handle_));
        }
        handle_ = nullptr;
    }

private:
    HANDLE handle_{};
};

[[nodiscard]] bool IsNameConflict(const DWORD status) noexcept {
    return status == DNS_ERROR_RCODE_YXDOMAIN || status == DNS_ERROR_RCODE_YXRRSET ||
           status == DNS_ERROR_RECORD_ALREADY_EXISTS || status == ERROR_ALREADY_EXISTS;
}

class DnsSdRegistration final {
public:
    DnsSdRegistration(const discovery::ServiceDefinition& service,
                      const discovery::NetworkInterfaceInfo& interface_info)
        : completion_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        std::vector<PCWSTR> keys;
        std::vector<PCWSTR> values;
        keys.reserve(service.txt_properties.size());
        values.reserve(service.txt_properties.size());
        for (const auto& property : service.txt_properties) {
            keys.push_back(property.key.c_str());
            values.push_back(property.value.c_str());
        }
        requested_fqdn_ = discovery::BuildServiceFqdn(service);
        IP4_ADDRESS address = interface_info.ipv4_address;
        instance_ = DnsServiceConstructInstance(
            requested_fqdn_.c_str(),
            service.host_name.c_str(),
            &address,
            nullptr,
            service.port,
            0U,
            0U,
            static_cast<DWORD>(keys.size()),
            keys.empty() ? nullptr : keys.data(),
            values.empty() ? nullptr : values.data());
        request_.Version = DNS_QUERY_REQUEST_VERSION1;
        request_.InterfaceIndex = interface_info.ipv4_interface_index;
        request_.pServiceInstance = instance_;
        request_.pRegisterCompletionCallback = &DnsSdRegistration::OnComplete;
        request_.pQueryContext = this;
        request_.hCredentials = nullptr;
        request_.unicastEnabled = FALSE;
    }

    ~DnsSdRegistration() {
        Deregister();
        if (instance_ != nullptr) {
            DnsServiceFreeInstance(instance_);
            instance_ = nullptr;
        }
    }

    DnsSdRegistration(const DnsSdRegistration&) = delete;
    DnsSdRegistration& operator=(const DnsSdRegistration&) = delete;

    [[nodiscard]] DWORD Register() noexcept {
        if (!completion_event_.IsValid() || instance_ == nullptr) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        {
            std::scoped_lock callback_lock{callback_mutex_};
            actual_fqdn_.clear();
        }
        callback_status_.store(ERROR_IO_PENDING, std::memory_order_release);
        const DWORD status = DnsServiceRegister(&request_, &cancel_);
        if (status != DNS_REQUEST_PENDING) {
            return status;
        }
        constexpr DWORD kRegistrationTimeoutMilliseconds = 10'000U;
        DWORD wait_status = WaitForSingleObject(completion_event_.Get(),
                                                kRegistrationTimeoutMilliseconds);
        if (wait_status == WAIT_TIMEOUT) {
            const DWORD cancel_status = DnsServiceRegisterCancel(&cancel_);
            if (cancel_status == ERROR_SUCCESS || cancel_status == ERROR_CANCELLED) {
                return ERROR_TIMEOUT;
            }
            wait_status = WaitForSingleObject(completion_event_.Get(), INFINITE);
            if (wait_status != WAIT_OBJECT_0) {
                return cancel_status;
            }
        }
        if (wait_status != WAIT_OBJECT_0) {
            return GetLastError();
        }
        const DWORD completion_status = callback_status_.load(std::memory_order_acquire);
        registered_ = completion_status == ERROR_SUCCESS;
        return completion_status;
    }

    [[nodiscard]] bool WasAutomaticallyRenamed() const {
        std::scoped_lock callback_lock{callback_mutex_};
        if (actual_fqdn_.empty()) {
            return false;
        }
        std::wstring_view requested = requested_fqdn_;
        std::wstring_view actual = actual_fqdn_;
        if (!requested.empty() && requested.back() == L'.') {
            requested.remove_suffix(1U);
        }
        if (!actual.empty() && actual.back() == L'.') {
            actual.remove_suffix(1U);
        }
        return requested.size() != actual.size() ||
               _wcsnicmp(requested.data(), actual.data(), requested.size()) != 0;
    }

    void Deregister() noexcept {
        if (!registered_) {
            return;
        }
        callback_status_.store(ERROR_IO_PENDING, std::memory_order_release);
        const DWORD status = DnsServiceDeRegister(&request_, nullptr);
        if (status == DNS_REQUEST_PENDING) {
            static_cast<void>(WaitForSingleObject(completion_event_.Get(), INFINITE));
        }
        registered_ = false;
    }

private:
    static void WINAPI OnComplete(const DWORD status,
                                  void* const context,
                                  DNS_SERVICE_INSTANCE* const callback_instance) noexcept {
        if (context == nullptr) {
            if (callback_instance != nullptr) {
                DnsServiceFreeInstance(callback_instance);
            }
            return;
        }
        auto& self = *static_cast<DnsSdRegistration*>(context);
        DWORD reported_status = status;
        if (status == ERROR_SUCCESS && callback_instance != nullptr &&
            callback_instance->pszInstanceName != nullptr) {
            try {
                std::scoped_lock callback_lock{self.callback_mutex_};
                self.actual_fqdn_ = callback_instance->pszInstanceName;
            } catch (...) {
                reported_status = ERROR_NOT_ENOUGH_MEMORY;
            }
        }
        if (callback_instance != nullptr) {
            DnsServiceFreeInstance(callback_instance);
        }
        self.callback_status_.store(reported_status, std::memory_order_release);
        static_cast<void>(SetEvent(self.completion_event_.Get()));
    }

    UniqueHandle completion_event_;
    DNS_SERVICE_INSTANCE* instance_{};
    DNS_SERVICE_REGISTER_REQUEST request_{};
    DNS_SERVICE_CANCEL cancel_{};
    std::atomic<DWORD> callback_status_{ERROR_IO_PENDING};
    mutable std::mutex callback_mutex_;
    std::wstring requested_fqdn_{};
    std::wstring actual_fqdn_{};
    bool registered_{};
};

}  // namespace

class WindowsDiscoveryService::Impl final {
public:
    Impl()
        : stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          interface_change_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~Impl() {
        Stop();
    }

    [[nodiscard]] bool Start(const discovery::DiscoveryConfig& requested_config) {
        std::scoped_lock lifecycle_lock{lifecycle_mutex_};
        if (started_) {
            return true;
        }
        if (!stop_event_.IsValid() || !interface_change_event_.IsValid() ||
            (!requested_config.advertise_raop && !requested_config.advertise_airplay) ||
            (requested_config.advertise_raop && requested_config.raop_port == 0U) ||
            (requested_config.advertise_airplay && requested_config.airplay_port == 0U)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }

        config_ = requested_config;
        config_.device_name = discovery::NormalizeDeviceName(config_.device_name);
        if (discovery::IsZeroDeviceId(config_.device_id)) {
            config_.device_id = WindowsNetworkInterfaceEnumerator::SystemDeviceId();
        }
        host_name_ = WindowsNetworkInterfaceEnumerator::LocalHostName();
        conflict_ordinal_ = 1U;
        static_cast<void>(ResetEvent(stop_event_.Get()));
        static_cast<void>(ResetEvent(interface_change_event_.Get()));

        const DWORD notify_status = NotifyIpInterfaceChange(AF_UNSPEC,
                                                             &Impl::OnInterfaceChange,
                                                             this,
                                                             FALSE,
                                                             &notification_handle_);
        if (notify_status != NO_ERROR) {
            SetLastError(notify_status);
            notification_handle_ = nullptr;
            return false;
        }

        {
            std::scoped_lock state_lock{state_mutex_};
            diagnostics_ = {};
            diagnostics_.running = true;
            diagnostics_.advertised_name = config_.device_name;
            initial_publication_complete_ = false;
        }
        started_ = true;
        try {
            worker_ = std::thread{&Impl::WorkerMain, this};
        } catch (...) {
            static_cast<void>(CancelMibChangeNotify2(notification_handle_));
            notification_handle_ = nullptr;
            started_ = false;
            std::scoped_lock state_lock{state_mutex_};
            diagnostics_.running = false;
            diagnostics_.last_error = ERROR_NOT_ENOUGH_MEMORY;
            return false;
        }

        std::unique_lock state_lock{state_mutex_};
        static_cast<void>(initial_publication_cv_.wait_for(
            state_lock,
            std::chrono::seconds(30),
            [this] { return initial_publication_complete_; }));
        return true;
    }

    void Stop() noexcept {
        std::scoped_lock lifecycle_lock{lifecycle_mutex_};
        if (!started_) {
            return;
        }
        if (notification_handle_ != nullptr) {
            static_cast<void>(CancelMibChangeNotify2(notification_handle_));
            notification_handle_ = nullptr;
        }
        static_cast<void>(SetEvent(stop_event_.Get()));
        if (worker_.joinable()) {
            worker_.join();
        }
        started_ = false;
    }

    [[nodiscard]] discovery::DiscoveryDiagnostics Diagnostics() const {
        std::scoped_lock state_lock{state_mutex_};
        return diagnostics_;
    }

private:
    static void NETIOAPI_API_ OnInterfaceChange(void* const context,
                                                MIB_IPINTERFACE_ROW*,
                                                MIB_NOTIFICATION_TYPE) noexcept {
        if (context != nullptr) {
            auto& self = *static_cast<Impl*>(context);
            static_cast<void>(SetEvent(self.interface_change_event_.Get()));
        }
    }

    void SetLastError(const DWORD error) {
        std::scoped_lock state_lock{state_mutex_};
        diagnostics_.last_error = error;
    }

    void WorkerMain() noexcept {
        try {
            Publish();
            {
                std::scoped_lock state_lock{state_mutex_};
                initial_publication_complete_ = true;
            }
            initial_publication_cv_.notify_all();

            const HANDLE events[] = {stop_event_.Get(), interface_change_event_.Get()};
            while (true) {
                const DWORD wait_status = WaitForMultipleObjects(2U, events, FALSE, INFINITE);
                if (wait_status == WAIT_OBJECT_0) {
                    break;
                }
                if (wait_status != WAIT_OBJECT_0 + 1U) {
                    SetLastError(GetLastError());
                    break;
                }
                if (WaitForSingleObject(stop_event_.Get(), 500U) == WAIT_OBJECT_0) {
                    break;
                }
                static_cast<void>(ResetEvent(interface_change_event_.Get()));
                {
                    std::scoped_lock state_lock{state_mutex_};
                    ++diagnostics_.network_change_count;
                }
                Publish();
            }
        } catch (...) {
            std::scoped_lock state_lock{state_mutex_};
            initial_publication_complete_ = true;
            ++diagnostics_.registration_failure_count;
            diagnostics_.last_error = ERROR_NOT_ENOUGH_MEMORY;
            initial_publication_cv_.notify_all();
        }

        registrations_.clear();
        std::scoped_lock state_lock{state_mutex_};
        diagnostics_.running = false;
        diagnostics_.active_interface_count = 0U;
        diagnostics_.registered_service_count = 0U;
    }

    void Publish() {
        registrations_.clear();
        const auto interfaces = WindowsNetworkInterfaceEnumerator::Enumerate(
            config_.include_virtual_interfaces);
        DWORD last_error = ERROR_SUCCESS;
        std::uint64_t failures = 0U;

        constexpr std::uint32_t kMaximumNameAttempts = 100U;
        for (std::uint32_t attempt = 0U; attempt < kMaximumNameAttempts; ++attempt) {
            const std::wstring advertised_name =
                discovery::MakeConflictResolvedName(config_.device_name, conflict_ordinal_);
            const auto services =
                discovery::BuildAirPlayServiceRecords(config_, advertised_name, host_name_);
            std::vector<std::unique_ptr<DnsSdRegistration>> pending;
            pending.reserve(interfaces.size() * services.size());
            bool conflict = false;

            for (const auto& interface_info : interfaces) {
                for (const auto& service : services) {
                    auto registration =
                        std::make_unique<DnsSdRegistration>(service, interface_info);
                    const DWORD status = registration->Register();
                    if (IsNameConflict(status)) {
                        conflict = true;
                        last_error = status;
                        break;
                    }
                    if (status != ERROR_SUCCESS) {
                        last_error = status;
                        ++failures;
                        continue;
                    }
                    if (registration->WasAutomaticallyRenamed()) {
                        conflict = true;
                        last_error = ERROR_ALREADY_EXISTS;
                        break;
                    }
                    pending.push_back(std::move(registration));
                }
                if (conflict) {
                    break;
                }
            }

            if (conflict) {
                pending.clear();
                ++conflict_ordinal_;
                std::scoped_lock state_lock{state_mutex_};
                ++diagnostics_.name_conflict_count;
                continue;
            }

            registrations_ = std::move(pending);
            std::scoped_lock state_lock{state_mutex_};
            diagnostics_.active_interface_count =
                static_cast<std::uint32_t>(interfaces.size());
            diagnostics_.registered_service_count =
                static_cast<std::uint32_t>(registrations_.size());
            ++diagnostics_.publication_generation;
            diagnostics_.registration_failure_count += failures;
            diagnostics_.advertised_name = advertised_name;
            diagnostics_.last_error = failures == 0U ? ERROR_SUCCESS : last_error;
            return;
        }

        std::scoped_lock state_lock{state_mutex_};
        diagnostics_.active_interface_count = static_cast<std::uint32_t>(interfaces.size());
        diagnostics_.registered_service_count = 0U;
        ++diagnostics_.publication_generation;
        diagnostics_.registration_failure_count += failures + 1U;
        diagnostics_.last_error = last_error == ERROR_SUCCESS ? ERROR_ALREADY_EXISTS : last_error;
    }

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex state_mutex_;
    std::condition_variable initial_publication_cv_;
    UniqueHandle stop_event_;
    UniqueHandle interface_change_event_;
    HANDLE notification_handle_{};
    std::thread worker_;
    discovery::DiscoveryConfig config_{};
    discovery::DiscoveryDiagnostics diagnostics_{};
    std::wstring host_name_{};
    std::vector<std::unique_ptr<DnsSdRegistration>> registrations_{};
    std::uint32_t conflict_ordinal_{1U};
    bool initial_publication_complete_{};
    bool started_{};
};

WindowsDiscoveryService::WindowsDiscoveryService() : impl_(std::make_unique<Impl>()) {}

WindowsDiscoveryService::~WindowsDiscoveryService() = default;

bool WindowsDiscoveryService::Start(const discovery::DiscoveryConfig& config) {
    return impl_->Start(config);
}

void WindowsDiscoveryService::Stop() noexcept {
    impl_->Stop();
}

discovery::DiscoveryDiagnostics WindowsDiscoveryService::Diagnostics() const {
    return impl_->Diagnostics();
}

}  // namespace airplaywin::windows::network
