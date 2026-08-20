#include "TestFramework.h"

#include <chrono>
#include <set>
#include <string>
#include <thread>

#include "platform/windows/audio/WindowsAudioDeviceEnumerator.h"
#include "platform/windows/timing/QpcClock.h"

void TestWindowsPlatform() {
    using airplaywin::windows::audio::WindowsAudioDeviceEnumerator;
    using airplaywin::windows::timing::QpcClock;

    APW_EXPECT(QpcClock::Frequency() > 0);
    const auto first_tick = QpcClock::Now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto second_tick = QpcClock::Now();
    APW_EXPECT(second_tick > first_tick);
    APW_EXPECT(QpcClock::ToSeconds(second_tick - first_tick) > 0.0);

    const auto devices = WindowsAudioDeviceEnumerator::EnumerateRenderDevices();
    std::set<std::wstring> device_ids;
    std::size_t default_count = 0U;
    for (const auto& device : devices) {
        APW_EXPECT(!device.id.empty());
        APW_EXPECT(!device.friendly_name.empty());
        APW_EXPECT(device_ids.insert(device.id).second);
        default_count += device.is_default ? 1U : 0U;
    }
    APW_EXPECT(default_count <= 1U);
}
