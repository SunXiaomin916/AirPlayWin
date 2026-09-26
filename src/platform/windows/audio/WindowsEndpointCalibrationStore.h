#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/group/EndpointCalibrationStore.h"

namespace airplaywin::windows::audio {

class WindowsEndpointCalibrationStore final
    : public airplaywin::group::IEndpointCalibrationStore {
public:
    explicit WindowsEndpointCalibrationStore(
        std::wstring registry_path =
            L"Software\\AirPlayWin\\EndpointCalibration");

    [[nodiscard]] bool Save(
        const airplaywin::group::EndpointCalibration& calibration) override;
    [[nodiscard]] std::optional<airplaywin::group::EndpointCalibration> Load(
        std::string_view endpoint_id) const override;
    bool Remove(std::string_view endpoint_id) override;

    [[nodiscard]] bool SaveWindowsEndpoint(
        std::wstring_view endpoint_id,
        std::int64_t offset_microseconds,
        airplaywin::group::EndpointCalibrationSource source);
    [[nodiscard]] std::optional<airplaywin::group::EndpointCalibration>
    LoadWindowsEndpoint(std::wstring_view endpoint_id) const;
    bool RemoveWindowsEndpoint(std::wstring_view endpoint_id);
    [[nodiscard]] std::uint32_t LastError() const noexcept;

    [[nodiscard]] static std::optional<std::string> ToUtf8(
        std::wstring_view value);

private:
    [[nodiscard]] static std::wstring ValueName(std::string_view endpoint_id);
    [[nodiscard]] static std::optional<std::wstring> ToWide(
        std::string_view value);

    std::wstring registry_path_{};
    mutable std::atomic<std::uint32_t> last_error_{0U};
};

}  // namespace airplaywin::windows::audio
