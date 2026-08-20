#include "TestFramework.h"

#include <array>

#include "core/audio/AudioEpoch.h"

void TestAudioEpoch() {
    using airplaywin::audio::AudioEpoch;
    using airplaywin::audio::AudioEpochReason;

    AudioEpoch epoch;
    APW_EXPECT(epoch.Current() == 1U);
    APW_EXPECT(epoch.IsCurrent(1U));
    APW_EXPECT(epoch.Advance(AudioEpochReason::Flush) == 2U);
    APW_EXPECT(epoch.LastReason() == AudioEpochReason::Flush);
    APW_EXPECT(!epoch.IsCurrent(1U));
    APW_EXPECT(epoch.Advance(AudioEpochReason::HardResync) == 3U);
    APW_EXPECT(epoch.LastReason() == AudioEpochReason::HardResync);

    constexpr std::array<AudioEpochReason, 6U> kTimelineChanges{
        AudioEpochReason::Flush,        AudioEpochReason::Seek,
        AudioEpochReason::FormatChange, AudioEpochReason::SenderReplace,
        AudioEpochReason::HardResync,   AudioEpochReason::SessionReset,
    };
    for (std::uint64_t iteration = 0U; iteration < 1'000U; ++iteration) {
        const auto reason = kTimelineChanges[iteration % kTimelineChanges.size()];
        const auto previous = epoch.Current();
        APW_EXPECT(epoch.Advance(reason) == previous + 1U);
        APW_EXPECT(epoch.LastReason() == reason);
        APW_EXPECT(!epoch.IsCurrent(previous));
    }
    APW_EXPECT(epoch.Current() == 1'003U);
}
