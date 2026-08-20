#include "TestFramework.h"

#include "platform/windows/audio/EndpointLatencyModel.h"

void TestEndpointLatencyModel() {
    using airplaywin::windows::audio::EndpointLatencyInput;
    using airplaywin::windows::audio::EndpointLatencyModel;

    const auto measured = EndpointLatencyModel::Estimate(EndpointLatencyInput{
        .sample_rate = 48'000U,
        .software_queue_frames = 480U,
        .endpoint_padding_frames = 240U,
        .engine_period_frames = 96U,
        .engine_latency_microseconds = 1'000U,
        .calibration_offset_microseconds = -500,
    });
    APW_EXPECT(measured.software_queue_microseconds == 10'000U);
    APW_EXPECT(measured.endpoint_padding_microseconds == 5'000U);
    APW_EXPECT(measured.engine_microseconds == 1'000U);
    APW_EXPECT(measured.total_microseconds == 15'500U);

    const auto period_fallback = EndpointLatencyModel::Estimate(EndpointLatencyInput{
        .sample_rate = 48'000U,
        .engine_period_frames = 96U,
        .calibration_offset_microseconds = 250,
    });
    APW_EXPECT(period_fallback.engine_microseconds == 2'000U);
    APW_EXPECT(period_fallback.total_microseconds == 2'250U);

    const auto clamped = EndpointLatencyModel::Estimate(EndpointLatencyInput{
        .sample_rate = 48'000U,
        .software_queue_frames = 48U,
        .calibration_offset_microseconds = -2'000,
    });
    APW_EXPECT(clamped.total_microseconds == 0U);
}
