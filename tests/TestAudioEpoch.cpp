#include "TestFramework.h"

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
}
