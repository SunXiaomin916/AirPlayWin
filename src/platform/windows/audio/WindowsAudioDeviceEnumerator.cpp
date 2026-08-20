#include "platform/windows/audio/WindowsAudioDeviceEnumerator.h"

#include <Windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

namespace airplaywin::windows::audio {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartment final {
public:
    ComApartment() noexcept {
        const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        usable_ = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
        uninitialize_ = SUCCEEDED(result);
    }

    ~ComApartment() {
        if (uninitialize_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool IsUsable() const noexcept { return usable_; }

private:
    bool usable_{false};
    bool uninitialize_{false};
};

[[nodiscard]] std::wstring DeviceId(IMMDevice* const device) {
    LPWSTR raw_id = nullptr;
    if (FAILED(device->GetId(&raw_id)) || raw_id == nullptr) {
        return {};
    }
    std::wstring id{raw_id};
    CoTaskMemFree(raw_id);
    return id;
}

[[nodiscard]] std::wstring FriendlyName(IMMDevice* const device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) {
        return {};
    }

    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

}  // namespace

std::vector<AudioDeviceInfo> WindowsAudioDeviceEnumerator::EnumerateRenderDevices() {
    ComApartment apartment;
    if (!apartment.IsUsable()) {
        return {};
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        return {};
    }

    const auto default_id = DefaultRenderDeviceId();
    ComPtr<IMMDeviceCollection> collection;
    constexpr DWORD kAllRelevantStates =
        DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT |
        DEVICE_STATE_UNPLUGGED;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, kAllRelevantStates, &collection))) {
        return {};
    }

    UINT count = 0U;
    if (FAILED(collection->GetCount(&count))) {
        return {};
    }

    std::vector<AudioDeviceInfo> devices;
    devices.reserve(count);
    for (UINT index = 0U; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device))) {
            continue;
        }
        DWORD state = 0U;
        if (FAILED(device->GetState(&state))) {
            continue;
        }
        auto id = DeviceId(device.Get());
        if (id.empty()) {
            continue;
        }
        auto name = FriendlyName(device.Get());
        if (name.empty()) {
            name = id;
        }
        devices.push_back(AudioDeviceInfo{
            .id = std::move(id),
            .friendly_name = std::move(name),
            .state = static_cast<std::uint32_t>(state),
            .is_default = false,
        });
        devices.back().is_default = devices.back().id == default_id;
    }

    std::ranges::sort(devices, [](const AudioDeviceInfo& left, const AudioDeviceInfo& right) {
        if (left.is_default != right.is_default) {
            return left.is_default;
        }
        return left.friendly_name < right.friendly_name;
    });
    return devices;
}

std::wstring WindowsAudioDeviceEnumerator::DefaultRenderDeviceId() {
    ComApartment apartment;
    if (!apartment.IsUsable()) {
        return {};
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) {
        return {};
    }
    return DeviceId(device.Get());
}

}  // namespace airplaywin::windows::audio
