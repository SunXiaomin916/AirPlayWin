#include "TestFramework.h"

#include <Windows.h>

#include <string>

#include "platform/windows/audio/WindowsEndpointCalibrationStore.h"

void TestWindowsEndpointCalibrationStore() {
    using airplaywin::group::EndpointCalibrationSource;
    using airplaywin::windows::audio::WindowsEndpointCalibrationStore;

    const std::wstring registry_path =
        L"Software\\AirPlayWin\\Tests\\EndpointCalibration-" +
        std::to_wstring(GetCurrentProcessId());
    WindowsEndpointCalibrationStore store{registry_path};
    const std::wstring endpoint = L"{0.0.0.00000000}.测试-USB-DAC";

    APW_EXPECT(store.SaveWindowsEndpoint(endpoint, -1'250,
                                         EndpointCalibrationSource::Measured));
    const auto loaded = store.LoadWindowsEndpoint(endpoint);
    APW_EXPECT(loaded.has_value());
    APW_EXPECT(loaded->offset_microseconds == -1'250);
    APW_EXPECT(loaded->source == EndpointCalibrationSource::Measured);
    APW_EXPECT(store.RemoveWindowsEndpoint(endpoint));
    APW_EXPECT(!store.LoadWindowsEndpoint(endpoint).has_value());
    APW_EXPECT(store.LastError() == ERROR_FILE_NOT_FOUND);

    static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, registry_path.c_str()));
}
