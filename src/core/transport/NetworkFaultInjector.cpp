#include "core/transport/NetworkFaultInjector.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace airplaywin::transport {

NetworkFaultInjector::NetworkFaultInjector(const NetworkFaultConfig config)
    : config_(config), random_state_(config.seed) {
    if (!config_.IsValid()) {
        throw std::invalid_argument("invalid network fault configuration");
    }
}

NetworkFaultDecision NetworkFaultInjector::Next(
    const std::uint64_t packet_index,
    const std::uint64_t source_time_microseconds) noexcept {
    NetworkFaultDecision result;
    ++diagnostics_.packets_evaluated;
    const bool burst_drop =
        config_.burst_period_packets != 0U &&
        packet_index >= config_.burst_period_packets - 1U &&
        (packet_index - (config_.burst_period_packets - 1U)) %
                config_.burst_period_packets <
            config_.burst_length_packets;
    const bool random_drop = Draw(config_.random_loss_basis_points);
    result.drop = burst_drop || random_drop;
    result.burst_drop = burst_drop;
    if (result.drop) {
        ++diagnostics_.dropped_packets;
        diagnostics_.burst_drops += burst_drop ? 1U : 0U;
        diagnostics_.random_drops += !burst_drop && random_drop ? 1U : 0U;
    } else {
        result.duplicate = Draw(config_.duplicate_basis_points);
        result.reorder = Draw(config_.reorder_basis_points);
        diagnostics_.duplicated_packets += result.duplicate ? 1U : 0U;
        diagnostics_.reordered_packets += result.reorder ? 1U : 0U;
    }

    std::int64_t signed_jitter = 0;
    if (config_.jitter_microseconds != 0U) {
        const auto width = config_.jitter_microseconds * 2U + 1U;
        signed_jitter = static_cast<std::int64_t>(Random() % width) -
                        static_cast<std::int64_t>(config_.jitter_microseconds);
    }
    const auto base_delay = static_cast<std::int64_t>(config_.base_latency_microseconds);
    auto delay = std::max<std::int64_t>(0, base_delay + signed_jitter);
    if (result.reorder &&
        delay <= std::numeric_limits<std::int64_t>::max() -
                     static_cast<std::int64_t>(config_.reorder_delay_microseconds)) {
        delay += static_cast<std::int64_t>(config_.reorder_delay_microseconds);
    }
    const auto delay_microseconds = static_cast<std::uint64_t>(delay);
    if (source_time_microseconds > std::numeric_limits<std::uint64_t>::max() -
                                       delay_microseconds) {
        result.delivery_time_microseconds = std::numeric_limits<std::uint64_t>::max();
    } else {
        result.delivery_time_microseconds = source_time_microseconds + delay_microseconds;
    }
    if (diagnostics_.packets_evaluated == 1U) {
        diagnostics_.minimum_delay_microseconds = delay_microseconds;
    } else {
        diagnostics_.minimum_delay_microseconds =
            std::min(diagnostics_.minimum_delay_microseconds, delay_microseconds);
    }
    diagnostics_.maximum_delay_microseconds =
        std::max(diagnostics_.maximum_delay_microseconds, delay_microseconds);
    return result;
}

NetworkFaultDiagnostics NetworkFaultInjector::Diagnostics() const noexcept {
    return diagnostics_;
}

void NetworkFaultInjector::Reset() noexcept {
    diagnostics_ = {};
    random_state_ = config_.seed;
}

std::uint64_t NetworkFaultInjector::Random() noexcept {
    random_state_ += 0x9E37'79B9'7F4A'7C15ULL;
    auto value = random_state_;
    value = (value ^ (value >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return value ^ (value >> 31U);
}

bool NetworkFaultInjector::Draw(const std::uint32_t basis_points) noexcept {
    return basis_points != 0U && Random() % 10'000U < basis_points;
}

}  // namespace airplaywin::transport
