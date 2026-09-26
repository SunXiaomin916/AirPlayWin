#include "core/group/EndpointCalibrationStore.h"

#include <algorithm>

namespace airplaywin::group {

bool EndpointCalibration::IsValid() const noexcept {
    return !endpoint_id.empty() && endpoint_id.size() <= 2'048U &&
           offset_microseconds >= -1'000'000LL &&
           offset_microseconds <= 1'000'000LL;
}

bool InMemoryEndpointCalibrationStore::Save(
    const EndpointCalibration& calibration) {
    if (!calibration.IsValid()) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    calibrations_.insert_or_assign(calibration.endpoint_id, calibration);
    return true;
}

std::optional<EndpointCalibration> InMemoryEndpointCalibrationStore::Load(
    const std::string_view endpoint_id) const {
    std::scoped_lock lock{mutex_};
    const auto found = calibrations_.find(std::string{endpoint_id});
    return found == calibrations_.end()
               ? std::nullopt
               : std::optional<EndpointCalibration>{found->second};
}

bool InMemoryEndpointCalibrationStore::Remove(
    const std::string_view endpoint_id) {
    std::scoped_lock lock{mutex_};
    return calibrations_.erase(std::string{endpoint_id}) != 0U;
}

std::vector<EndpointCalibration> InMemoryEndpointCalibrationStore::Snapshot() const {
    std::scoped_lock lock{mutex_};
    std::vector<EndpointCalibration> result;
    result.reserve(calibrations_.size());
    for (const auto& [id, calibration] : calibrations_) {
        static_cast<void>(id);
        result.push_back(calibration);
    }
    std::ranges::sort(result, {}, &EndpointCalibration::endpoint_id);
    return result;
}

}  // namespace airplaywin::group
