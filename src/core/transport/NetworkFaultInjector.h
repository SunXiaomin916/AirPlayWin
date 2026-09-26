#pragma once

#include <cstdint>

namespace airplaywin::transport {

struct NetworkFaultConfig final {
    std::uint64_t base_latency_microseconds{0U};
    std::uint64_t jitter_microseconds{0U};
    std::uint32_t random_loss_basis_points{0U};
    std::uint32_t duplicate_basis_points{0U};
    std::uint32_t reorder_basis_points{0U};
    std::uint32_t burst_period_packets{0U};
    std::uint32_t burst_length_packets{0U};
    std::uint64_t reorder_delay_microseconds{0U};
    std::uint64_t seed{1U};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return base_latency_microseconds <= 10'000'000U &&
               jitter_microseconds <= 100'000U &&
               random_loss_basis_points <= 10'000U &&
               duplicate_basis_points <= 10'000U &&
               reorder_basis_points <= 10'000U &&
               ((burst_period_packets == 0U && burst_length_packets == 0U) ||
                (burst_period_packets != 0U && burst_length_packets != 0U &&
                 burst_length_packets <= burst_period_packets)) &&
               reorder_delay_microseconds <= 10'000'000U;
    }
};

struct NetworkFaultDecision final {
    bool drop{false};
    bool duplicate{false};
    bool reorder{false};
    bool burst_drop{false};
    std::uint64_t delivery_time_microseconds{0U};
};

struct NetworkFaultDiagnostics final {
    std::uint64_t packets_evaluated{0U};
    std::uint64_t dropped_packets{0U};
    std::uint64_t random_drops{0U};
    std::uint64_t burst_drops{0U};
    std::uint64_t duplicated_packets{0U};
    std::uint64_t reordered_packets{0U};
    std::uint64_t minimum_delay_microseconds{0U};
    std::uint64_t maximum_delay_microseconds{0U};
};

class NetworkFaultInjector final {
public:
    explicit NetworkFaultInjector(NetworkFaultConfig config);

    [[nodiscard]] NetworkFaultDecision Next(
        std::uint64_t packet_index,
        std::uint64_t source_time_microseconds) noexcept;
    [[nodiscard]] NetworkFaultDiagnostics Diagnostics() const noexcept;
    void Reset() noexcept;

private:
    [[nodiscard]] std::uint64_t Random() noexcept;
    [[nodiscard]] bool Draw(std::uint32_t basis_points) noexcept;

    NetworkFaultConfig config_{};
    NetworkFaultDiagnostics diagnostics_{};
    std::uint64_t random_state_{1U};
};

}  // namespace airplaywin::transport
