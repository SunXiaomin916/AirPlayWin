#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace airplaywin::group {

enum class EndpointCalibrationSource : std::uint8_t {
    Manual,
    Measured,
};

struct EndpointCalibration final {
    std::string endpoint_id{};
    std::int64_t offset_microseconds{0};
    EndpointCalibrationSource source{EndpointCalibrationSource::Manual};

    [[nodiscard]] bool IsValid() const noexcept;
};

class IEndpointCalibrationStore {
public:
    virtual ~IEndpointCalibrationStore() = default;

    [[nodiscard]] virtual bool Save(const EndpointCalibration& calibration) = 0;
    [[nodiscard]] virtual std::optional<EndpointCalibration> Load(
        std::string_view endpoint_id) const = 0;
    virtual bool Remove(std::string_view endpoint_id) = 0;
};

class InMemoryEndpointCalibrationStore final : public IEndpointCalibrationStore {
public:
    [[nodiscard]] bool Save(const EndpointCalibration& calibration) override;
    [[nodiscard]] std::optional<EndpointCalibration> Load(
        std::string_view endpoint_id) const override;
    bool Remove(std::string_view endpoint_id) override;
    [[nodiscard]] std::vector<EndpointCalibration> Snapshot() const;

private:
    mutable std::mutex mutex_{};
    std::unordered_map<std::string, EndpointCalibration> calibrations_{};
};

}  // namespace airplaywin::group
