#include "TestFramework.h"

#include <memory>

#include "core/group/EndpointCalibrationStore.h"

void TestEndpointCalibrationStore() {
    using airplaywin::group::EndpointCalibration;
    using airplaywin::group::EndpointCalibrationSource;
    using airplaywin::group::InMemoryEndpointCalibrationStore;

    InMemoryEndpointCalibrationStore store;
    APW_EXPECT(!store.Save({}));
    APW_EXPECT(store.Save({.endpoint_id = "usb-dac-1",
                           .offset_microseconds = 1'250,
                           .source = EndpointCalibrationSource::Measured}));
    auto loaded = store.Load("usb-dac-1");
    APW_EXPECT(loaded.has_value());
    APW_EXPECT(loaded->offset_microseconds == 1'250);
    APW_EXPECT(loaded->source == EndpointCalibrationSource::Measured);
    APW_EXPECT(store.Save({.endpoint_id = "usb-dac-1",
                           .offset_microseconds = -750,
                           .source = EndpointCalibrationSource::Manual}));
    loaded = store.Load("usb-dac-1");
    APW_EXPECT(loaded.has_value());
    APW_EXPECT(loaded->offset_microseconds == -750);
    APW_EXPECT(store.Save({.endpoint_id = "hdmi-2", .offset_microseconds = 22'000}));
    const auto snapshot = store.Snapshot();
    APW_EXPECT(snapshot.size() == 2U);
    APW_EXPECT(snapshot[0].endpoint_id == "hdmi-2");
    APW_EXPECT(snapshot[1].endpoint_id == "usb-dac-1");
    APW_EXPECT(store.Remove("usb-dac-1"));
    APW_EXPECT(!store.Load("usb-dac-1").has_value());
    APW_EXPECT(!store.Remove("missing"));
    APW_EXPECT(!store.Save(EndpointCalibration{
        .endpoint_id = "invalid", .offset_microseconds = 1'000'001}));
}
