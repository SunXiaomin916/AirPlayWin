#include "TestFramework.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "core/timing/DisciplinedRtpTimingEngine.h"
#include "core/timing/IMonotonicClock.h"
#include "core/timing/PtpClockDomain.h"
#include "core/timing/PtpPacket.h"
#include "core/timing/RtpPtpPhaseTimeline.h"

namespace {

class ManualClock final : public airplaywin::timing::IMonotonicClock {
public:
    ManualClock(const std::int64_t now, const std::int64_t frequency) noexcept
        : now_(now), frequency_(frequency) {}

    [[nodiscard]] std::int64_t Now() const noexcept override { return now_; }
    [[nodiscard]] std::int64_t Frequency() const noexcept override { return frequency_; }

private:
    std::int64_t now_{0};
    std::int64_t frequency_{0};
};

void WriteU16(std::span<std::byte> bytes, const std::size_t offset,
              const std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 1U] = static_cast<std::byte>(value);
}

void WriteU32(std::span<std::byte> bytes, const std::size_t offset,
              const std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>(value >> 24U);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 16U);
    bytes[offset + 2U] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 3U] = static_cast<std::byte>(value);
}

void WriteU64(std::span<std::byte> bytes, const std::size_t offset,
              const std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>(value >> ((7U - index) * 8U));
    }
}

[[nodiscard]] std::array<std::byte, 44U> BuildPtp(
    const std::uint8_t message_type, const std::uint16_t sequence,
    const std::uint64_t timestamp_nanoseconds, const bool two_step = false,
    const std::int64_t correction_nanoseconds = 0) {
    std::array<std::byte, 44U> bytes{};
    bytes[0U] = static_cast<std::byte>(message_type);
    bytes[1U] = std::byte{2U};
    WriteU16(bytes, 2U, static_cast<std::uint16_t>(bytes.size()));
    bytes[4U] = std::byte{7U};
    WriteU16(bytes, 6U, two_step ? 0x0200U : 0U);
    WriteU64(bytes, 8U, std::bit_cast<std::uint64_t>(correction_nanoseconds * 65'536LL));
    WriteU64(bytes, 20U, 0x0102'0304'0506'0708ULL);
    WriteU16(bytes, 28U, 1U);
    WriteU16(bytes, 30U, sequence);
    const auto seconds = timestamp_nanoseconds / 1'000'000'000U;
    const auto nanoseconds = static_cast<std::uint32_t>(
        timestamp_nanoseconds % 1'000'000'000U);
    for (std::size_t index = 0U; index < 6U; ++index) {
        bytes[34U + index] =
            static_cast<std::byte>(seconds >> ((5U - index) * 8U));
    }
    WriteU32(bytes, 40U, nanoseconds);
    return bytes;
}

}  // namespace

void TestPtpTiming() {
    using airplaywin::timing::ClockServoConfig;
    using airplaywin::timing::ClockServoState;
    using airplaywin::timing::ParsePtpPacket;
    using airplaywin::timing::PtpClockDomain;
    using airplaywin::timing::PtpMessageType;

    const auto parsed = ParsePtpPacket(BuildPtp(0U, 4U, 12'345'678'901ULL,
                                                 false, 1'250LL));
    APW_EXPECT(parsed.has_value());
    APW_EXPECT(parsed->message_type == PtpMessageType::Sync);
    APW_EXPECT(parsed->domain_number == 7U);
    APW_EXPECT(parsed->source_clock_identity == 0x0102'0304'0506'0708ULL);
    APW_EXPECT(parsed->origin_timestamp_nanoseconds == 12'345'678'901ULL);
    APW_EXPECT(parsed->correction_nanoseconds == 1'250LL);
    std::array<std::byte, 10U> short_packet{};
    APW_EXPECT(!ParsePtpPacket(short_packet).has_value());

    PtpClockDomain domain{ClockServoConfig{
        .local_clock_frequency = 1'000'000LL,
        .minimum_lock_samples = 8U,
        .relock_samples = 3U,
        .maximum_rtt_microseconds = 20'000U,
        .outlier_threshold_microseconds = 3'000U,
        .hard_resync_threshold_microseconds = 50'000U,
        .holdover_after_microseconds = 1'500'000U,
        .unlock_after_microseconds = 5'000'000U,
        .maximum_absolute_drift_ppm = 500.0,
        .drift_filter_alpha = 0.25,
        .offset_filter_alpha = 0.125,
        .maximum_rate_slew_ppm_per_second = 25.0,
    }};
    constexpr std::uint64_t remote_base = 1'700'000'000'000'000'000ULL;
    constexpr std::int64_t local_base = 20'000'000LL;
    for (std::uint16_t sequence = 0U; sequence < 20U; ++sequence) {
        const auto remote = remote_base + static_cast<std::uint64_t>(sequence) *
                                              1'000'000'000ULL;
        const auto local = local_base + static_cast<std::int64_t>(sequence) * 1'000'050LL;
        if ((sequence & 1U) == 0U) {
            domain.OnDatagram(BuildPtp(0U, sequence, remote), local);
        } else {
            domain.OnDatagram(BuildPtp(0U, sequence, 0U, true), local);
            domain.OnDatagram(BuildPtp(8U, sequence, remote), local + 50LL);
        }
    }
    auto diagnostics = domain.Diagnostics();
    APW_EXPECT(diagnostics.received_datagrams == 30U);
    APW_EXPECT(diagnostics.sync_messages == 20U);
    APW_EXPECT(diagnostics.follow_up_messages == 10U);
    APW_EXPECT(diagnostics.completed_samples == 20U);
    APW_EXPECT(diagnostics.servo.state == ClockServoState::Locked);
    APW_EXPECT(std::abs(diagnostics.servo.drift_ppm - 50.0) < 1.0);
    const auto mapped = domain.RemoteToLocalQpc(remote_base + 25'000'000'000ULL);
    APW_EXPECT(mapped.has_value());
    APW_EXPECT(std::abs(*mapped - (local_base + 25'001'250LL)) < 100LL);

    const auto engine_now = local_base + 19LL * 1'000'050LL;
    airplaywin::timing::DisciplinedRtpTimingEngine engine{
        {.remote_clock_rate = 8'000U, .target_buffer_milliseconds = 100U},
        std::shared_ptr<PtpClockDomain>{&domain, [](PtpClockDomain*) {}},
        std::make_unique<ManualClock>(engine_now, 1'000'000LL)};
    engine.Reset(1'000U);
    APW_EXPECT(engine.RemoteToLocalQpc(1'000U) == engine_now + 100'000LL);
    const auto next_target = engine.RemoteToLocalQpc(9'000U);
    APW_EXPECT(next_target.has_value());
    APW_EXPECT(std::abs(*next_target - (engine_now + 1'100'050LL)) < 100LL);
    APW_EXPECT(engine.Diagnostics().locked);
    APW_EXPECT(std::abs(engine.RateCorrection() - (1.0 / 1.00005)) < 0.00001);

    auto phase_timeline = std::make_shared<airplaywin::timing::RtpPtpPhaseTimeline>();
    APW_EXPECT(phase_timeline->Publish({
        .rtp_timestamp = 1'000U,
        .remote_ptp_nanoseconds = remote_base + 19'000'000'000ULL,
        .sample_rate = 8'000U,
        .session_epoch = 3U,
        .master_clock_identity = 0x0102'0304'0506'0708ULL,
    }));
    airplaywin::timing::DisciplinedRtpTimingEngine group_engine{
        {.remote_clock_rate = 8'000U,
         .target_buffer_milliseconds = 100U,
         .endpoint_latency_offset_microseconds = 1'000,
         .require_phase_anchor = true},
        std::shared_ptr<PtpClockDomain>{&domain, [](PtpClockDomain*) {}},
        std::make_unique<ManualClock>(engine_now, 1'000'000LL), phase_timeline};
    group_engine.Reset(1'000U);
    const auto group_target = group_engine.RemoteToLocalQpc(1'000U);
    APW_EXPECT(group_target.has_value());
    APW_EXPECT(std::abs(*group_target - (engine_now - 1'000LL)) < 100LL);
    APW_EXPECT(group_engine.Diagnostics().absolute_phase_active);
    APW_EXPECT(group_engine.Diagnostics().phase_session_epoch == 3U);
    APW_EXPECT(group_engine.RequiresMappedTarget());
    group_engine.Reset(1'000U);
    APW_EXPECT(!group_engine.Diagnostics().absolute_phase_active);
    APW_EXPECT(group_engine.RemoteToLocalQpc(1'000U).has_value());

    auto missing_timeline = std::make_shared<airplaywin::timing::RtpPtpPhaseTimeline>();
    airplaywin::timing::DisciplinedRtpTimingEngine waiting_engine{
        {.remote_clock_rate = 8'000U,
         .target_buffer_milliseconds = 100U,
         .require_phase_anchor = true},
        std::shared_ptr<PtpClockDomain>{&domain, [](PtpClockDomain*) {}},
        std::make_unique<ManualClock>(engine_now, 1'000'000LL), missing_timeline};
    APW_EXPECT(!waiting_engine.RemoteToLocalQpc(1'000U).has_value());
    APW_EXPECT(waiting_engine.Diagnostics().phase_mapping_failures == 1U);

    domain.OnDatagram(BuildPtp(8U, 99U, remote_base), local_base);
    diagnostics = domain.Diagnostics();
    APW_EXPECT(diagnostics.unmatched_follow_up_messages == 1U);
    domain.OnDatagram(short_packet, local_base);
    APW_EXPECT(domain.Diagnostics().invalid_datagrams == 1U);
}
