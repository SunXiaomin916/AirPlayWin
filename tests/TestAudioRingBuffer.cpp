#include "TestFramework.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

#include "core/audio/AudioRingBuffer.h"

void TestAudioRingBuffer() {
    using airplaywin::audio::AudioRingBuffer;

    AudioRingBuffer ring{8U, 2U};
    const std::array<float, 12U> first{0.0F, 1.0F, 2.0F, 3.0F, 4.0F, 5.0F,
                                      6.0F, 7.0F, 8.0F, 9.0F, 10.0F, 11.0F};
    APW_EXPECT(ring.Write(first, 6U));
    APW_EXPECT(ring.AvailableFrames() == 6U);

    std::array<float, 8U> first_read{};
    APW_EXPECT(ring.Read(first_read, 4U) == 4U);
    for (std::size_t index = 0U; index < first_read.size(); ++index) {
        APW_EXPECT(first_read[index] == first[index]);
    }

    const std::array<float, 8U> second{12.0F, 13.0F, 14.0F, 15.0F,
                                      16.0F, 17.0F, 18.0F, 19.0F};
    APW_EXPECT(ring.Write(second, 4U));
    std::array<float, 12U> wrapped_read{};
    APW_EXPECT(ring.Read(wrapped_read, 6U) == 6U);
    for (std::size_t index = 0U; index < 4U; ++index) {
        APW_EXPECT(wrapped_read[index] == first[index + 8U]);
    }
    for (std::size_t index = 0U; index < second.size(); ++index) {
        APW_EXPECT(wrapped_read[index + 4U] == second[index]);
    }

    APW_EXPECT(ring.Write(first, 6U));
    ring.Reset();
    APW_EXPECT(ring.AvailableFrames() == 0U);
    std::array<float, 2U> discard_probe{};
    APW_EXPECT(ring.Read(discard_probe, 1U) == 0U);
    APW_EXPECT(ring.FreeFrames() == ring.CapacityFrames());

    AudioRingBuffer concurrent_ring{256U, 1U};
    constexpr std::uint32_t kFrameCount = 50'000U;
    std::atomic<bool> failed{false};
    std::thread producer([&]() {
        for (std::uint32_t value = 0U; value < kFrameCount; ++value) {
            const std::array<float, 1U> sample{static_cast<float>(value)};
            while (!concurrent_ring.Write(sample, 1U)) {
                std::this_thread::yield();
            }
        }
    });
    std::thread consumer([&]() {
        for (std::uint32_t expected = 0U; expected < kFrameCount; ++expected) {
            std::array<float, 1U> sample{};
            while (concurrent_ring.Read(sample, 1U) == 0U) {
                std::this_thread::yield();
            }
            if (sample[0] != static_cast<float>(expected)) {
                failed.store(true, std::memory_order_release);
            }
        }
    });
    producer.join();
    consumer.join();
    APW_EXPECT(!failed.load(std::memory_order_acquire));
}
