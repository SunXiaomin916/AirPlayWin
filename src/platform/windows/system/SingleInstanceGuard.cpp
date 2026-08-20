#include "platform/windows/system/SingleInstanceGuard.h"

#include <string>

namespace airplaywin::windows::system {

SingleInstanceGuard::SingleInstanceGuard(const std::wstring_view mutex_name) noexcept {
    if (mutex_name.empty()) {
        last_error_ = ERROR_INVALID_PARAMETER;
        return;
    }
    try {
        const std::wstring name{mutex_name};
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (mutex_ == nullptr) {
            last_error_ = GetLastError();
            return;
        }
        const auto create_error = GetLastError();
        if (create_error == ERROR_ALREADY_EXISTS) {
            already_running_ = true;
            static_cast<void>(CloseHandle(mutex_));
            mutex_ = nullptr;
            last_error_ = ERROR_ALREADY_EXISTS;
        }
    } catch (...) {
        last_error_ = ERROR_NOT_ENOUGH_MEMORY;
    }
}

SingleInstanceGuard::~SingleInstanceGuard() {
    if (mutex_ != nullptr) {
        static_cast<void>(CloseHandle(mutex_));
    }
}

bool SingleInstanceGuard::Acquired() const noexcept {
    return mutex_ != nullptr;
}

bool SingleInstanceGuard::AlreadyRunning() const noexcept {
    return already_running_;
}

std::uint32_t SingleInstanceGuard::LastError() const noexcept {
    return last_error_;
}

}  // namespace airplaywin::windows::system
