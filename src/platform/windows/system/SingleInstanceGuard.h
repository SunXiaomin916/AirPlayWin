#pragma once

#include <Windows.h>

#include <cstdint>
#include <string_view>

namespace airplaywin::windows::system {

class SingleInstanceGuard final {
public:
    explicit SingleInstanceGuard(std::wstring_view mutex_name) noexcept;
    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    [[nodiscard]] bool Acquired() const noexcept;
    [[nodiscard]] bool AlreadyRunning() const noexcept;
    [[nodiscard]] std::uint32_t LastError() const noexcept;

private:
    HANDLE mutex_{nullptr};
    std::uint32_t last_error_{ERROR_SUCCESS};
    bool already_running_{false};
};

}  // namespace airplaywin::windows::system
