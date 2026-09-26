#include "TestFramework.h"

#include <cstdint>

#include "core/timing/RtpPtpPhaseTimeline.h"

void TestGroupTimeline() {
    using airplaywin::timing::RtpPtpPhaseAnchor;
    using airplaywin::timing::RtpPtpPhaseTimeline;

    RtpPtpPhaseTimeline timeline;
    APW_EXPECT(!timeline.RtpToRemotePtpNanoseconds(0U).has_value());
    APW_EXPECT(!timeline.Publish({}));
    const RtpPtpPhaseAnchor anchor{
        .rtp_timestamp = 0xFFFF'FF00U,
        .remote_ptp_nanoseconds = 1'700'000'000'000'000'000ULL,
        .sample_rate = 48'000U,
        .session_epoch = 7U,
        .master_clock_identity = 0x1234U,
    };
    APW_EXPECT(timeline.Publish(anchor));
    const auto wrapped = timeline.RtpToRemotePtpNanoseconds(0x0000'00E0U);
    APW_EXPECT(wrapped.has_value());
    APW_EXPECT(*wrapped == anchor.remote_ptp_nanoseconds + 10'000'000ULL);
    const auto reverse = timeline.RemotePtpNanosecondsToRtp(*wrapped);
    APW_EXPECT(reverse.has_value());
    APW_EXPECT(*reverse == 0x0000'00E0U);
    APW_EXPECT(!timeline.Publish(RtpPtpPhaseAnchor{
        .rtp_timestamp = 1U,
        .remote_ptp_nanoseconds = anchor.remote_ptp_nanoseconds,
        .sample_rate = 48'000U,
        .session_epoch = 6U,
    }));
    const auto diagnostics = timeline.Diagnostics();
    APW_EXPECT(diagnostics.anchored);
    APW_EXPECT(diagnostics.published_anchors == 1U);
    APW_EXPECT(diagnostics.mapped_timestamps == 2U);
    timeline.Clear();
    APW_EXPECT(!timeline.Diagnostics().anchored);
}
