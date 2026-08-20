#include "platform/windows/audio/WasapiAudioOutput.h"

#include <Windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/audio/AudioDiagnostics.h"
#include "core/audio/AudioRingBuffer.h"
#include "core/audio/AudioTransitionGuard.h"
#include "platform/windows/audio/EndpointLatencyModel.h"

namespace airplaywin::windows::audio {
namespace {

using Microsoft::WRL::ComPtr;
using airplaywin::audio::AudioClientPath;
using airplaywin::audio::AudioEndpointSampleFormat;
using airplaywin::audio::AudioOutputMode;
using airplaywin::audio::AudioTransition;
using airplaywin::audio::AudioTransitionState;

constexpr std::uint64_t kReferenceTimeUnitsPerSecond = 10'000'000U;

[[nodiscard]] DWORD ChannelMask(const std::uint16_t channel_count) noexcept {
    switch (channel_count) {
    case 1U:
        return SPEAKER_FRONT_CENTER;
    case 2U:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 4U:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT |
               SPEAKER_BACK_RIGHT;
    case 6U:
        return KSAUDIO_SPEAKER_5POINT1;
    case 8U:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default:
        return 0U;
    }
}

[[nodiscard]] WAVEFORMATEXTENSIBLE MakeFloatFormat(
    const airplaywin::audio::AudioFormat& format) noexcept {
    WAVEFORMATEXTENSIBLE wave{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = format.channel_count;
    wave.Format.nSamplesPerSec = format.sample_rate;
    wave.Format.wBitsPerSample = 32U;
    wave.Format.nBlockAlign = static_cast<WORD>(format.channel_count * sizeof(float));
    wave.Format.nAvgBytesPerSec = format.sample_rate * wave.Format.nBlockAlign;
    wave.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    wave.Samples.wValidBitsPerSample = 32U;
    wave.dwChannelMask = ChannelMask(format.channel_count);
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return wave;
}

[[nodiscard]] WAVEFORMATEXTENSIBLE MakePcm16Format(
    const airplaywin::audio::AudioFormat& format) noexcept {
    WAVEFORMATEXTENSIBLE wave{};
    wave.Format.wFormatTag = format.channel_count <= 2U ? WAVE_FORMAT_PCM
                                                        : WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = format.channel_count;
    wave.Format.nSamplesPerSec = format.sample_rate;
    wave.Format.wBitsPerSample = 16U;
    wave.Format.nBlockAlign = static_cast<WORD>(format.channel_count * sizeof(std::int16_t));
    wave.Format.nAvgBytesPerSec = format.sample_rate * wave.Format.nBlockAlign;
    wave.Format.cbSize = format.channel_count <= 2U
                             ? 0U
                             : static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) -
                                                 sizeof(WAVEFORMATEX));
    wave.Samples.wValidBitsPerSample = 16U;
    wave.dwChannelMask = ChannelMask(format.channel_count);
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return wave;
}

[[nodiscard]] std::uint32_t RingCapacityFrames(
    const airplaywin::audio::AudioFormat& format,
    const std::uint32_t milliseconds) noexcept {
    const auto requested =
        (static_cast<std::uint64_t>(format.sample_rate) * milliseconds + 999U) / 1'000U;
    const auto minimum = static_cast<std::uint64_t>(format.sample_rate) / 10U;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        std::max(requested, minimum), std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] std::uint32_t ReferenceTimeToFrames(
    const REFERENCE_TIME duration,
    const std::uint32_t sample_rate) noexcept {
    if (duration <= 0 || sample_rate == 0U) {
        return 0U;
    }
    const auto frames =
        (static_cast<std::uint64_t>(duration) * sample_rate +
         kReferenceTimeUnitsPerSecond - 1U) /
        kReferenceTimeUnitsPerSecond;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        frames, std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] REFERENCE_TIME FramesToReferenceTime(
    const std::uint32_t frames,
    const std::uint32_t sample_rate) noexcept {
    if (frames == 0U || sample_rate == 0U) {
        return 0;
    }
    return static_cast<REFERENCE_TIME>(
        (static_cast<std::uint64_t>(frames) * kReferenceTimeUnitsPerSecond +
         sample_rate - 1U) /
        sample_rate);
}

}  // namespace

class WasapiAudioOutput::Impl final {
public:
    explicit Impl(WasapiOutputOptions options)
        : options_(std::move(options)),
          audio_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
          shutdown_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          reopen_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        if (audio_event_ == nullptr || shutdown_event_ == nullptr || reopen_event_ == nullptr) {
            const auto error = static_cast<int>(GetLastError());
            CloseHandles();
            throw std::system_error(error, std::system_category(), "CreateEventW failed");
        }
        if (options_.ring_capacity_milliseconds < 100U) {
            options_.ring_capacity_milliseconds = 100U;
        }
        const auto minimum_queue_milliseconds = options_.low_latency ? 2U : 10U;
        options_.target_queue_milliseconds = std::clamp(
            options_.target_queue_milliseconds, minimum_queue_milliseconds,
            options_.ring_capacity_milliseconds);
    }

    ~Impl() {
        Close();
        CloseHandles();
    }

    bool Open(const airplaywin::audio::AudioFormat& format) {
        std::scoped_lock lock(control_mutex_);
        if (!format.IsValid() || opened_.load(std::memory_order_acquire)) {
            return false;
        }

        const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
            last_error_.store(com_result, std::memory_order_release);
            return false;
        }
        com_initialized_here_ = SUCCEEDED(com_result);
        format_ = format;
        target_queue_frames_ = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(format.sample_rate) *
                 options_.target_queue_milliseconds +
             999U) /
            1'000U);

        try {
            ring_ = std::make_unique<airplaywin::audio::AudioRingBuffer>(
                RingCapacityFrames(format, options_.ring_capacity_milliseconds),
                format.channel_count);
            guard_ = std::make_unique<airplaywin::audio::AudioTransitionGuard>(
                airplaywin::audio::AudioTransitionConfig{
                    .sample_rate = format.sample_rate,
                    .fade_in_milliseconds = 10.0F,
                    .fade_out_milliseconds = 10.0F,
                    .volume_ramp_milliseconds = 20.0F,
                    .prewarm_milliseconds = 10.0F,
                    .maximum_pcm_amplitude = 1.0F,
                    .dc_offset_threshold = 0.15F,
                });
        } catch (...) {
            CleanupComApartment();
            return false;
        }

        auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&device_enumerator_));
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            ResetOwnedAudioObjects();
            CleanupComApartment();
            return false;
        }

        notification_client_.Attach(new NotificationClient(this));
        result = device_enumerator_->RegisterEndpointNotificationCallback(
            notification_client_.Get());
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            notification_client_.Reset();
            device_enumerator_.Reset();
            ResetOwnedAudioObjects();
            CleanupComApartment();
            return false;
        }
        notification_registered_ = true;

        if (!OpenDevice()) {
            UnregisterNotification();
            notification_client_.Reset();
            device_enumerator_.Reset();
            ResetOwnedAudioObjects();
            CleanupComApartment();
            return false;
        }

        ResetEvent(shutdown_event_);
        current_epoch_.store(1U, std::memory_order_release);
        opened_.store(true, std::memory_order_release);
        return true;
    }

    bool Start() {
        std::scoped_lock lock(control_mutex_);
        if (!opened_.load(std::memory_order_acquire) || guard_ == nullptr) {
            return false;
        }
        if (worker_.joinable()) {
            guard_->Request(AudioTransition::Start);
            requested_playing_.store(true, std::memory_order_release);
            return true;
        }

        ResetEvent(shutdown_event_);
        requested_playing_.store(true, std::memory_order_release);
        guard_->Request(AudioTransition::Start);
        const auto launch = [this]() -> bool {
            try {
                worker_ = std::thread(&Impl::RenderThreadMain, this);
            } catch (...) {
                last_error_.store(E_OUTOFMEMORY, std::memory_order_release);
                return false;
            }
            if (StartPhysicalClient()) {
                return true;
            }
            SetEvent(shutdown_event_);
            worker_.join();
            ResetEvent(shutdown_event_);
            StopPhysicalClient();
            return false;
        };
        const auto initial_rendered_frames = diagnostics_.RenderedFrames();
        if (!launch()) {
            requested_playing_.store(false, std::memory_order_release);
            guard_->Request(AudioTransition::Stop);
            return false;
        }
        if (active_output_mode_.load(std::memory_order_acquire) !=
            AudioOutputMode::Exclusive) {
            return true;
        }

        const auto wakeup_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (diagnostics_.RenderedFrames() == initial_rendered_frames &&
               std::chrono::steady_clock::now() < wakeup_deadline) {
            std::this_thread::yield();
        }
        if (diagnostics_.RenderedFrames() != initial_rendered_frames) {
            return true;
        }

        const auto timeout_error = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        exclusive_start_timeouts_.fetch_add(1U, std::memory_order_relaxed);
        SetEvent(shutdown_event_);
        worker_.join();
        ResetEvent(shutdown_event_);
        StopPhysicalClient();
        if (!options_.allow_shared_fallback) {
            last_error_.store(timeout_error, std::memory_order_release);
            requested_playing_.store(false, std::memory_order_release);
            guard_->Request(AudioTransition::Stop);
            return false;
        }

        force_shared_fallback_.store(true, std::memory_order_release);
        force_shared_fallback_error_.store(timeout_error, std::memory_order_release);
        if (!OpenDevice() || !launch()) {
            requested_playing_.store(false, std::memory_order_release);
            guard_->Request(AudioTransition::Stop);
            return false;
        }
        return true;
    }

    bool Write(const airplaywin::audio::AudioBuffer& buffer) noexcept {
        if (!opened_.load(std::memory_order_acquire) || ring_ == nullptr || guard_ == nullptr ||
            !buffer.IsValid() || buffer.sample_rate != format_.sample_rate ||
            buffer.channel_count != format_.channel_count) {
            return false;
        }
        if (buffer.epoch_id != current_epoch_.load(std::memory_order_acquire)) {
            diagnostics_.RecordStaleEpochBuffer();
            return false;
        }

        const auto available_frames = ring_->AvailableFrames();
        if (guard_->State() == AudioTransitionState::SafeMute &&
            !guard_->HasPendingCommand() && available_frames >= target_queue_frames_) {
            guard_->Request(AudioTransition::Resume);
        }
        if (available_frames >= std::max(target_queue_frames_, buffer.frame_count)) {
            return false;
        }

        const auto written = ring_->Write(buffer.interleaved_samples, buffer.frame_count);
        if (written &&
            buffer.epoch_id != current_epoch_.load(std::memory_order_acquire)) {
            // An epoch switch raced this producer after its initial check. The
            // post-publish reset prevents that old PCM from crossing timelines.
            ring_->Reset();
            diagnostics_.RecordStaleEpochBuffer();
            return false;
        }
        if (written && guard_->State() == AudioTransitionState::SafeMute &&
            ring_->AvailableFrames() >= target_queue_frames_) {
            guard_->Request(AudioTransition::Resume);
        }
        return written;
    }

    void Pause() noexcept {
        requested_playing_.store(false, std::memory_order_release);
        if (guard_ != nullptr) {
            guard_->Request(AudioTransition::Pause);
        }
    }

    void Resume() noexcept {
        requested_playing_.store(true, std::memory_order_release);
        if (guard_ != nullptr) {
            guard_->Request(AudioTransition::Resume);
        }
    }

    void Stop() noexcept {
        requested_playing_.store(false, std::memory_order_release);
        if (ring_ != nullptr) {
            ring_->Reset();
        }
        if (guard_ != nullptr) {
            guard_->Request(AudioTransition::Stop);
        }
    }

    void Flush() noexcept {
        if (ring_ != nullptr) {
            ring_->Reset();
        }
        if (guard_ != nullptr) {
            guard_->Request(AudioTransition::Flush);
        }
    }

    void BeginEpoch(const std::uint64_t epoch_id, const AudioTransition transition) noexcept {
        if (epoch_id == 0U) {
            return;
        }
        if (guard_ != nullptr && transition != AudioTransition::None) {
            guard_->Request(transition);
        }
        current_epoch_.store(epoch_id, std::memory_order_release);
        if (ring_ != nullptr) {
            ring_->Reset();
        }
    }

    void SetVolume(const float linear_gain) noexcept {
        if (guard_ != nullptr) {
            guard_->SetTargetVolume(linear_gain);
        }
    }

    void Close() noexcept {
        std::scoped_lock lock(control_mutex_);
        if (worker_.joinable() && guard_ != nullptr) {
            if (ring_ != nullptr) {
                ring_->Reset();
            }
            guard_->Request(AudioTransition::Stop);
            const auto fade_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(50);
            while (guard_->State() != AudioTransitionState::Stopped &&
                   std::chrono::steady_clock::now() < fade_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        opened_.store(false, std::memory_order_release);
        requested_playing_.store(false, std::memory_order_release);
        if (shutdown_event_ != nullptr) {
            SetEvent(shutdown_event_);
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        StopPhysicalClient();
        UnregisterNotification();
        notification_client_.Reset();
        device_enumerator_.Reset();
        ResetOwnedAudioObjects();
        CleanupComApartment();
    }

    [[nodiscard]] airplaywin::audio::AudioDiagnosticsSnapshot Diagnostics() const {
        const auto click_pop = guard_ != nullptr ? guard_->ClickPop()
                                                 : airplaywin::audio::ClickPopDiagnostics{};
        airplaywin::audio::AudioDiagnosticsSnapshot snapshot{
            .current_buffer_depth_frames =
                current_buffer_depth_frames_.load(std::memory_order_relaxed),
            .underrun_count = diagnostics_.UnderrunCount(),
            .rendered_frames = diagnostics_.RenderedFrames(),
            .dropped_stale_epoch_buffers = diagnostics_.DroppedStaleEpochBuffers(),
            .invalid_numeric_samples = guard_ != nullptr ? guard_->InvalidNumericSamples() : 0U,
            .clipped_samples = guard_ != nullptr ? guard_->ClippedSamples() : 0U,
            .dc_offset_events = guard_ != nullptr ? guard_->DcOffsetEvents() : 0U,
            .transition_requests = guard_ != nullptr ? guard_->TransitionRequests() : 0U,
            .fade_in_events = guard_ != nullptr ? guard_->FadeInEvents() : 0U,
            .fade_out_events = guard_ != nullptr ? guard_->FadeOutEvents() : 0U,
            .safe_mute_events = guard_ != nullptr ? guard_->SafeMuteEvents() : 0U,
            .hard_resync_events = guard_ != nullptr ? guard_->HardResyncEvents() : 0U,
            .underrun_transition_events =
                guard_ != nullptr ? guard_->UnderrunTransitionEvents() : 0U,
            .click_pop_analyzed_frames = click_pop.analyzed_frames,
            .click_pop_events = click_pop.transient_events,
            .click_pop_last_event_frame = click_pop.last_event_frame,
            .click_pop_maximum_step = click_pop.maximum_sample_step,
            .click_pop_recent_peak = click_pop.recent_peak_step,
            .output_latency_microseconds =
                output_latency_microseconds_.load(std::memory_order_relaxed),
            .software_queue_latency_microseconds =
                software_queue_latency_microseconds_.load(std::memory_order_relaxed),
            .endpoint_padding_latency_microseconds =
                endpoint_padding_latency_microseconds_.load(std::memory_order_relaxed),
            .engine_latency_microseconds =
                engine_latency_microseconds_.load(std::memory_order_relaxed),
            .endpoint_calibration_offset_microseconds =
                options_.endpoint_calibration_offset_microseconds,
            .endpoint_buffer_frames =
                published_endpoint_buffer_frames_.load(std::memory_order_relaxed),
            .engine_period_frames = engine_period_frames_.load(std::memory_order_relaxed),
            .queue_target_frames =
                published_target_queue_frames_.load(std::memory_order_relaxed),
            .requested_output_mode = options_.output_mode,
            .active_output_mode = active_output_mode_.load(std::memory_order_relaxed),
            .audio_client_path = audio_client_path_.load(std::memory_order_relaxed),
            .endpoint_sample_format =
                endpoint_sample_format_.load(std::memory_order_relaxed),
            .low_latency_requested = options_.low_latency,
            .low_latency_active = low_latency_active_.load(std::memory_order_relaxed),
            .output_mode_fallback = output_mode_fallback_.load(std::memory_order_relaxed),
            .output_mode_fallback_error = static_cast<std::uint32_t>(
                output_mode_fallback_error_.load(std::memory_order_relaxed)),
            .render_wakeup_count = render_wakeup_count_.load(std::memory_order_relaxed),
            .exclusive_start_timeouts =
                exclusive_start_timeouts_.load(std::memory_order_relaxed),
            .device_switch_events = device_switch_events_.load(std::memory_order_relaxed),
            .device_recovery_attempts =
                device_recovery_attempts_.load(std::memory_order_relaxed),
            .device_recovery_successes =
                device_recovery_successes_.load(std::memory_order_relaxed),
            .device_recovery_failures =
                device_recovery_failures_.load(std::memory_order_relaxed),
            .current_device_id = {},
            .current_sample_rate = format_.sample_rate,
            .current_audio_epoch = current_epoch_.load(std::memory_order_acquire),
            .last_output_error =
                static_cast<std::uint32_t>(last_error_.load(std::memory_order_relaxed)),
            .output_recovering = output_recovering_.load(std::memory_order_acquire),
            .transition_state =
                guard_ != nullptr ? guard_->State() : AudioTransitionState::Stopped,
        };
        {
            std::scoped_lock lock(device_id_mutex_);
            snapshot.current_device_id = current_device_id_;
        }
        return snapshot;
    }

private:
    class NotificationClient final : public IMMNotificationClient {
    public:
        explicit NotificationClient(Impl* const owner) noexcept : owner_(owner) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(const IID& interface_id,
                                                 void** const object) noexcept override {
            if (object == nullptr) {
                return E_POINTER;
            }
            if (interface_id == __uuidof(IUnknown) ||
                interface_id == __uuidof(IMMNotificationClient)) {
                *object = static_cast<IMMNotificationClient*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() noexcept override {
            return reference_count_.fetch_add(1U, std::memory_order_relaxed) + 1U;
        }

        ULONG STDMETHODCALLTYPE Release() noexcept override {
            const auto remaining =
                reference_count_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
            if (remaining == 0U) {
                delete this;
            }
            return remaining;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR device_id, DWORD) noexcept override {
            RequestReopenIfRelevant(device_id);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR device_id) noexcept override {
            RequestReopenIfRelevant(device_id);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR device_id) noexcept override {
            RequestReopenIfRelevant(device_id);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(const EDataFlow flow,
                                                         const ERole role,
                                                         LPCWSTR) noexcept override {
            auto* const owner = owner_.load(std::memory_order_acquire);
            if (owner != nullptr && owner->options_.follow_default_device && flow == eRender &&
                (role == eConsole || role == eMultimedia)) {
                owner->RequestReopen();
            }
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,
                                                         const PROPERTYKEY) noexcept override {
            return S_OK;
        }

        void DetachOwner() noexcept { owner_.store(nullptr, std::memory_order_release); }

    private:
        void RequestReopen() noexcept {
            auto* const owner = owner_.load(std::memory_order_acquire);
            if (owner != nullptr) {
                owner->RequestReopen();
            }
        }

        void RequestReopenIfRelevant(LPCWSTR device_id) noexcept {
            auto* const owner = owner_.load(std::memory_order_acquire);
            if (owner != nullptr && owner->IsRelevantEndpoint(device_id)) {
                owner->RequestReopen();
            }
        }

        std::atomic<ULONG> reference_count_{1U};
        std::atomic<Impl*> owner_;
    };

    enum class RecoveryState : std::uint8_t {
        Normal,
        FadingOut,
        Reopening,
    };

    void RequestReopen() noexcept {
        if (opened_.load(std::memory_order_acquire) && reopen_event_ != nullptr) {
            SetEvent(reopen_event_);
        }
    }

    [[nodiscard]] bool IsRelevantEndpoint(LPCWSTR device_id) const {
        if (device_id == nullptr) {
            return false;
        }
        if (!options_.follow_default_device && options_.device_id == device_id) {
            return true;
        }
        std::scoped_lock lock(device_id_mutex_);
        return current_device_id_ == device_id;
    }

    [[nodiscard]] static HRESULT ActivateClient(
        IMMDevice* const device,
        ComPtr<IAudioClient>& client,
        ComPtr<IAudioClient3>& client3) noexcept {
        client.Reset();
        client3.Reset();
        const auto result = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf()));
        if (SUCCEEDED(result)) {
            static_cast<void>(client.As(&client3));
        }
        return result;
    }

    [[nodiscard]] static HRESULT InitializeLegacyShared(
        IAudioClient* const client,
        const WAVEFORMATEX* const wave_format) noexcept {
        constexpr DWORD kSharedFlags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        return client->Initialize(AUDCLNT_SHAREMODE_SHARED, kSharedFlags, 0, 0,
                                  wave_format, nullptr);
    }

    [[nodiscard]] static HRESULT InitializeAudioClient3Shared(
        IAudioClient3* const client,
        const WAVEFORMATEX* const wave_format,
        std::uint32_t& period_frames) noexcept {
        if (client == nullptr) {
            return E_NOINTERFACE;
        }
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.bIsOffload = FALSE;
        properties.eCategory = AudioCategory_Media;
        auto result = client->SetClientProperties(&properties);
        if (FAILED(result)) {
            return result;
        }
        UINT32 default_period = 0U;
        UINT32 fundamental_period = 0U;
        UINT32 minimum_period = 0U;
        UINT32 maximum_period = 0U;
        result = client->GetSharedModeEnginePeriod(
            wave_format, &default_period, &fundamental_period, &minimum_period,
            &maximum_period);
        if (FAILED(result) || minimum_period == 0U || maximum_period < minimum_period) {
            return FAILED(result) ? result : E_FAIL;
        }
        period_frames = minimum_period;
        // IAudioClient3's low-latency initializer accepts the event-driven shared
        // stream flags, but not the legacy auto-conversion flags. The queried
        // format must therefore be natively supported by the endpoint engine.
        constexpr DWORD kSharedFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        return client->InitializeSharedAudioStream(kSharedFlags, period_frames, wave_format,
                                                   nullptr);
    }

    [[nodiscard]] HRESULT InitializeExclusive(
        IMMDevice* const device,
        ComPtr<IAudioClient>& client,
        ComPtr<IAudioClient3>& client3,
        const WAVEFORMATEX* const wave_format,
        std::uint32_t& period_frames) const noexcept {
        auto result = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, wave_format,
                                                nullptr);
        if (result != S_OK) {
            return result == S_FALSE ? AUDCLNT_E_UNSUPPORTED_FORMAT : result;
        }
        REFERENCE_TIME default_period = 0;
        REFERENCE_TIME minimum_period = 0;
        result = client->GetDevicePeriod(&default_period, &minimum_period);
        if (FAILED(result)) {
            return result;
        }
        auto requested_period = options_.low_latency && minimum_period > 0
                                    ? minimum_period
                                    : default_period;
        if (requested_period <= 0) {
            return E_FAIL;
        }
        constexpr DWORD kExclusiveFlags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
        result = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, kExclusiveFlags,
                                    requested_period, requested_period, wave_format, nullptr);
        if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 aligned_frames = 0U;
            const auto size_result = client->GetBufferSize(&aligned_frames);
            if (FAILED(size_result) || aligned_frames == 0U) {
                return FAILED(size_result) ? size_result : E_FAIL;
            }
            result = ActivateClient(device, client, client3);
            if (FAILED(result)) {
                return result;
            }
            requested_period = FramesToReferenceTime(aligned_frames, format_.sample_rate);
            result = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, kExclusiveFlags,
                                        requested_period, requested_period, wave_format,
                                        nullptr);
            if (SUCCEEDED(result)) {
                period_frames = aligned_frames;
            }
            return result;
        }
        if (SUCCEEDED(result)) {
            period_frames = ReferenceTimeToFrames(requested_period, format_.sample_rate);
        }
        return result;
    }

    [[nodiscard]] bool OpenDevice() noexcept {
        if (device_enumerator_ == nullptr || audio_event_ == nullptr) {
            return false;
        }

        ComPtr<IMMDevice> next_device;
        HRESULT result = E_FAIL;
        if (!options_.device_id.empty() && !options_.follow_default_device) {
            result = device_enumerator_->GetDevice(options_.device_id.c_str(), &next_device);
        } else {
            result = device_enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                                  &next_device);
        }
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            return false;
        }

        ComPtr<IAudioClient> next_client;
        ComPtr<IAudioClient3> next_client3;
        result = ActivateClient(next_device.Get(), next_client, next_client3);
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            return false;
        }

        auto wave_format = MakeFloatFormat(format_);
        auto pcm16_wave_format = MakePcm16Format(format_);
        const auto forced_shared = force_shared_fallback_.load(std::memory_order_acquire);
        auto active_mode = forced_shared ? AudioOutputMode::Shared : options_.output_mode;
        auto client_path = AudioClientPath::Legacy;
        auto endpoint_sample_format = AudioEndpointSampleFormat::Float32;
        auto low_latency_active = false;
        auto fallback_used = forced_shared;
        HRESULT fallback_error = forced_shared
                                     ? force_shared_fallback_error_.load(
                                           std::memory_order_acquire)
                                     : S_OK;
        std::uint32_t next_engine_period_frames = 0U;

        if (active_mode == AudioOutputMode::Exclusive) {
            result = InitializeExclusive(next_device.Get(), next_client, next_client3,
                                         &wave_format.Format,
                                         next_engine_period_frames);
            if (result == AUDCLNT_E_UNSUPPORTED_FORMAT) {
                result = ActivateClient(next_device.Get(), next_client, next_client3);
                if (SUCCEEDED(result)) {
                    result = InitializeExclusive(next_device.Get(), next_client,
                                                 next_client3,
                                                 &pcm16_wave_format.Format,
                                                 next_engine_period_frames);
                    if (SUCCEEDED(result)) {
                        endpoint_sample_format = AudioEndpointSampleFormat::Pcm16;
                    }
                }
            }
            if (FAILED(result) && options_.allow_shared_fallback) {
                fallback_used = true;
                fallback_error = result;
                active_mode = AudioOutputMode::Shared;
                endpoint_sample_format = AudioEndpointSampleFormat::Float32;
                result = ActivateClient(next_device.Get(), next_client, next_client3);
            }
        }
        if (SUCCEEDED(result) && active_mode == AudioOutputMode::Shared) {
            if (options_.low_latency) {
                result = InitializeAudioClient3Shared(next_client3.Get(), &wave_format.Format,
                                                      next_engine_period_frames);
                if (SUCCEEDED(result)) {
                    client_path = AudioClientPath::AudioClient3;
                    low_latency_active = true;
                } else {
                    if (!fallback_used) {
                        fallback_used = true;
                        fallback_error = result;
                    }
                    result = ActivateClient(next_device.Get(), next_client, next_client3);
                    if (SUCCEEDED(result)) {
                        result = InitializeLegacyShared(next_client.Get(),
                                                        &wave_format.Format);
                    }
                }
            } else {
                result = InitializeLegacyShared(next_client.Get(), &wave_format.Format);
            }
        } else if (SUCCEEDED(result) && active_mode == AudioOutputMode::Exclusive) {
            low_latency_active = options_.low_latency;
        }
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            return false;
        }
        result = next_client->SetEventHandle(audio_event_);
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            return false;
        }

        UINT32 next_buffer_frames = 0U;
        result = next_client->GetBufferSize(&next_buffer_frames);
        if (FAILED(result) || next_buffer_frames == 0U) {
            last_error_.store(FAILED(result) ? result : E_FAIL, std::memory_order_release);
            return false;
        }

        ComPtr<IAudioRenderClient> next_render_client;
        result = next_client->GetService(IID_PPV_ARGS(&next_render_client));
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            return false;
        }

        if (endpoint_sample_format == AudioEndpointSampleFormat::Pcm16) {
            try {
                render_scratch_.resize(static_cast<std::size_t>(next_buffer_frames) *
                                       format_.channel_count);
            } catch (...) {
                last_error_.store(E_OUTOFMEMORY, std::memory_order_release);
                return false;
            }
        } else {
            render_scratch_.clear();
        }

        REFERENCE_TIME default_period = 0;
        REFERENCE_TIME minimum_period = 0;
        result = next_client->GetDevicePeriod(&default_period, &minimum_period);
        if (FAILED(result)) {
            default_period = 0;
        }
        if (next_engine_period_frames == 0U) {
            next_engine_period_frames =
                ReferenceTimeToFrames(default_period, format_.sample_rate);
        }
        REFERENCE_TIME stream_latency = 0;
        result = next_client->GetStreamLatency(&stream_latency);
        if (FAILED(result)) {
            stream_latency = 0;
        }

        LPWSTR raw_device_id = nullptr;
        result = next_device->GetId(&raw_device_id);
        if (FAILED(result) || raw_device_id == nullptr) {
            last_error_.store(FAILED(result) ? result : E_FAIL, std::memory_order_release);
            return false;
        }
        std::wstring next_device_id{raw_device_id};
        CoTaskMemFree(raw_device_id);

        endpoint_device_ = std::move(next_device);
        audio_client_ = std::move(next_client);
        render_client_ = std::move(next_render_client);
        endpoint_buffer_frames_ = next_buffer_frames;
        published_endpoint_buffer_frames_.store(next_buffer_frames,
                                                std::memory_order_release);
        const auto configured_queue_frames = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(format_.sample_rate) *
                 options_.target_queue_milliseconds +
             999U) /
            1'000U);
        target_queue_frames_ = std::max(configured_queue_frames, next_buffer_frames);
        if (active_mode == AudioOutputMode::Exclusive || low_latency_active) {
            target_queue_frames_ = std::max(configured_queue_frames,
                                            next_engine_period_frames);
        }
        published_target_queue_frames_.store(target_queue_frames_,
                                             std::memory_order_release);
        engine_period_frames_.store(next_engine_period_frames, std::memory_order_release);
        const auto measured_engine_latency =
            stream_latency > 0
                ? static_cast<std::uint64_t>(stream_latency / 10)
                : static_cast<std::uint64_t>(next_engine_period_frames) * 1'000'000U /
                      format_.sample_rate;
        engine_latency_microseconds_.store(measured_engine_latency,
                                           std::memory_order_release);
        active_output_mode_.store(active_mode, std::memory_order_release);
        audio_client_path_.store(client_path, std::memory_order_release);
        endpoint_sample_format_.store(endpoint_sample_format, std::memory_order_release);
        low_latency_active_.store(low_latency_active, std::memory_order_release);
        output_mode_fallback_.store(fallback_used, std::memory_order_release);
        output_mode_fallback_error_.store(fallback_error, std::memory_order_release);
        {
            std::scoped_lock lock(device_id_mutex_);
            current_device_id_ = std::move(next_device_id);
        }
        last_error_.store(S_OK, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool PrimeSilence() noexcept {
        if (render_client_ == nullptr || endpoint_buffer_frames_ == 0U) {
            return false;
        }
        BYTE* buffer = nullptr;
        auto result = render_client_->GetBuffer(endpoint_buffer_frames_, &buffer);
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            return false;
        }
        DWORD flags = AUDCLNT_BUFFERFLAGS_SILENT;
        if (active_output_mode_.load(std::memory_order_relaxed) ==
            AudioOutputMode::Exclusive) {
            const auto bytes_per_sample =
                endpoint_sample_format_.load(std::memory_order_relaxed) ==
                        AudioEndpointSampleFormat::Pcm16
                    ? sizeof(std::int16_t)
                    : sizeof(float);
            std::memset(buffer, 0, static_cast<std::size_t>(endpoint_buffer_frames_) *
                                       format_.channel_count * bytes_per_sample);
            flags = 0U;
        }
        result = render_client_->ReleaseBuffer(endpoint_buffer_frames_, flags);
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool StartPhysicalClient() noexcept {
        if (audio_client_ == nullptr || !PrimeSilence()) {
            return false;
        }
        const auto result = audio_client_->Start();
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            return false;
        }
        physical_client_started_ = true;
        return true;
    }

    void StopPhysicalClient() noexcept {
        if (audio_client_ != nullptr && physical_client_started_) {
            const auto stop_result = audio_client_->Stop();
            if (FAILED(stop_result)) {
                last_error_.store(stop_result, std::memory_order_release);
            }
            physical_client_started_ = false;
        }
        render_client_.Reset();
        audio_client_.Reset();
        endpoint_device_.Reset();
        endpoint_buffer_frames_ = 0U;
        published_endpoint_buffer_frames_.store(0U, std::memory_order_release);
    }

    void RenderThreadMain() noexcept {
        const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const auto uninitialize = SUCCEEDED(com_result);
        DWORD task_index = 0U;
        const auto mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        if (mmcss != nullptr) {
            static_cast<void>(AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH));
        }

        RecoveryState recovery = RecoveryState::Normal;
        auto fade_deadline = std::chrono::steady_clock::time_point{};
        const std::array<HANDLE, 3U> events{shutdown_event_, reopen_event_, audio_event_};

        bool stop_requested = false;
        while (!stop_requested) {
            DWORD timeout = INFINITE;
            if (recovery == RecoveryState::FadingOut) {
                timeout = 10U;
            } else if (recovery == RecoveryState::Reopening) {
                timeout = 1'000U;
            }

            const auto wait_result =
                WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE,
                                       timeout);
            if (wait_result == WAIT_OBJECT_0) {
                stop_requested = true;
                continue;
            }
            if (wait_result == WAIT_OBJECT_0 + 1U) {
                if (recovery == RecoveryState::Normal) {
                    device_switch_events_.fetch_add(1U, std::memory_order_relaxed);
                    output_recovering_.store(true, std::memory_order_release);
                    if (guard_ != nullptr) {
                        guard_->Request(AudioTransition::DeviceSwitch);
                    }
                    if (ring_ != nullptr) {
                        ring_->Reset();
                    }
                    fade_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(50);
                    recovery = RecoveryState::FadingOut;
                }
            } else if (wait_result == WAIT_OBJECT_0 + 2U &&
                       recovery != RecoveryState::Reopening) {
                render_wakeup_count_.fetch_add(1U, std::memory_order_relaxed);
                if (!RenderOnce()) {
                    SetEvent(reopen_event_);
                }
            } else if (wait_result == WAIT_FAILED) {
                last_error_.store(HRESULT_FROM_WIN32(GetLastError()), std::memory_order_release);
                stop_requested = true;
                continue;
            }

            if (recovery == RecoveryState::FadingOut &&
                ((guard_ != nullptr && guard_->State() == AudioTransitionState::DeviceMuted) ||
                 std::chrono::steady_clock::now() >= fade_deadline)) {
                StopPhysicalClient();
                recovery = RecoverDevice() ? RecoveryState::Normal : RecoveryState::Reopening;
            } else if (recovery == RecoveryState::Reopening && wait_result == WAIT_TIMEOUT) {
                recovery = RecoverDevice() ? RecoveryState::Normal : RecoveryState::Reopening;
            }
        }

        if (mmcss != nullptr) {
            static_cast<void>(AvRevertMmThreadCharacteristics(mmcss));
        }
        if (uninitialize) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool RecoverDevice() noexcept {
        device_recovery_attempts_.fetch_add(1U, std::memory_order_relaxed);
        if (!OpenDevice()) {
            device_recovery_failures_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        if (!StartPhysicalClient()) {
            StopPhysicalClient();
            device_recovery_failures_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        if (guard_ != nullptr) {
            guard_->Request(requested_playing_.load(std::memory_order_acquire)
                                ? AudioTransition::Start
                                : AudioTransition::Pause);
        }
        device_recovery_successes_.fetch_add(1U, std::memory_order_relaxed);
        output_recovering_.store(false, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool RenderOnce() noexcept {
        if (audio_client_ == nullptr || render_client_ == nullptr || ring_ == nullptr ||
            guard_ == nullptr || endpoint_buffer_frames_ == 0U) {
            return false;
        }

        UINT32 padding_frames = 0U;
        HRESULT result = S_OK;
        if (active_output_mode_.load(std::memory_order_relaxed) !=
            AudioOutputMode::Exclusive) {
            result = audio_client_->GetCurrentPadding(&padding_frames);
            if (FAILED(result) || padding_frames > endpoint_buffer_frames_) {
                last_error_.store(FAILED(result) ? result : E_FAIL,
                                  std::memory_order_release);
                return false;
            }
        }
        const auto writable_frames = endpoint_buffer_frames_ - padding_frames;
        if (writable_frames == 0U) {
            return true;
        }

        BYTE* raw_buffer = nullptr;
        result = render_client_->GetBuffer(writable_frames, &raw_buffer);
        if (FAILED(result) || raw_buffer == nullptr) {
            last_error_.store(FAILED(result) ? result : E_FAIL, std::memory_order_release);
            return false;
        }

        const auto sample_count = static_cast<std::size_t>(writable_frames) *
                                  static_cast<std::size_t>(format_.channel_count);
        const auto endpoint_sample_format =
            endpoint_sample_format_.load(std::memory_order_relaxed);
        std::span<float> output;
        if (endpoint_sample_format == AudioEndpointSampleFormat::Pcm16) {
            if (render_scratch_.size() < sample_count) {
                static_cast<void>(render_client_->ReleaseBuffer(
                    writable_frames, AUDCLNT_BUFFERFLAGS_SILENT));
                last_error_.store(E_FAIL, std::memory_order_release);
                return false;
            }
            output = std::span<float>{render_scratch_.data(), sample_count};
        } else {
            output = std::span<float>{reinterpret_cast<float*>(raw_buffer), sample_count};
        }
        const auto state_before_render = guard_->State();
        const auto frames_read =
            guard_->ShouldHoldInput() ? 0U : ring_->Read(output, writable_frames);
        if (frames_read < writable_frames) {
            const auto valid_samples = static_cast<std::size_t>(frames_read) *
                                       static_cast<std::size_t>(format_.channel_count);
            std::fill(output.begin() + static_cast<std::ptrdiff_t>(valid_samples), output.end(),
                      0.0F);
            if (state_before_render == AudioTransitionState::Audible ||
                state_before_render == AudioTransitionState::FadingIn) {
                if (!guard_->HasPendingCommand()) {
                    diagnostics_.RecordUnderrun();
                }
            }
        }

        guard_->Process(output, writable_frames, format_.channel_count,
                        frames_read < writable_frames);
        if (endpoint_sample_format == AudioEndpointSampleFormat::Pcm16) {
            auto* const pcm = reinterpret_cast<std::int16_t*>(raw_buffer);
            for (std::size_t index = 0U; index < sample_count; ++index) {
                const auto sample = std::clamp(output[index], -1.0F, 1.0F);
                const auto scaled = sample >= 0.0F ? sample * 32'767.0F
                                                   : sample * 32'768.0F;
                pcm[index] = static_cast<std::int16_t>(scaled);
            }
        }
        result = render_client_->ReleaseBuffer(writable_frames, 0U);
        if (FAILED(result)) {
            last_error_.store(result, std::memory_order_release);
            return false;
        }

        diagnostics_.RecordRenderedFrames(writable_frames);
        const auto depth = ring_->AvailableFrames();
        current_buffer_depth_frames_.store(depth, std::memory_order_relaxed);
        const auto latency = EndpointLatencyModel::Estimate(EndpointLatencyInput{
            .sample_rate = format_.sample_rate,
            .software_queue_frames = depth,
            .endpoint_padding_frames = padding_frames,
            .engine_period_frames = engine_period_frames_.load(std::memory_order_relaxed),
            .engine_latency_microseconds =
                engine_latency_microseconds_.load(std::memory_order_relaxed),
            .calibration_offset_microseconds =
                options_.endpoint_calibration_offset_microseconds,
        });
        software_queue_latency_microseconds_.store(
            latency.software_queue_microseconds, std::memory_order_relaxed);
        endpoint_padding_latency_microseconds_.store(
            latency.endpoint_padding_microseconds, std::memory_order_relaxed);
        output_latency_microseconds_.store(latency.total_microseconds,
                                          std::memory_order_relaxed);
        return true;
    }

    void UnregisterNotification() noexcept {
        if (notification_client_ != nullptr) {
            notification_client_->DetachOwner();
        }
        if (notification_registered_ && device_enumerator_ != nullptr &&
            notification_client_ != nullptr) {
            const auto result = device_enumerator_->UnregisterEndpointNotificationCallback(
                notification_client_.Get());
            if (FAILED(result)) {
                last_error_.store(result, std::memory_order_release);
            }
        }
        notification_registered_ = false;
    }

    void ResetOwnedAudioObjects() noexcept {
        render_client_.Reset();
        audio_client_.Reset();
        endpoint_device_.Reset();
        ring_.reset();
        guard_.reset();
        render_scratch_.clear();
        endpoint_buffer_frames_ = 0U;
        published_endpoint_buffer_frames_.store(0U, std::memory_order_relaxed);
        current_buffer_depth_frames_.store(0U, std::memory_order_relaxed);
        output_latency_microseconds_.store(0U, std::memory_order_relaxed);
        software_queue_latency_microseconds_.store(0U, std::memory_order_relaxed);
        endpoint_padding_latency_microseconds_.store(0U, std::memory_order_relaxed);
        engine_period_frames_.store(0U, std::memory_order_relaxed);
        published_target_queue_frames_.store(0U, std::memory_order_relaxed);
        engine_latency_microseconds_.store(0U, std::memory_order_relaxed);
        active_output_mode_.store(AudioOutputMode::Shared, std::memory_order_relaxed);
        audio_client_path_.store(AudioClientPath::Legacy, std::memory_order_relaxed);
        endpoint_sample_format_.store(AudioEndpointSampleFormat::Float32,
                                      std::memory_order_relaxed);
        low_latency_active_.store(false, std::memory_order_relaxed);
        output_mode_fallback_.store(false, std::memory_order_relaxed);
        output_mode_fallback_error_.store(S_OK, std::memory_order_relaxed);
        force_shared_fallback_.store(false, std::memory_order_relaxed);
        force_shared_fallback_error_.store(S_OK, std::memory_order_relaxed);
        output_recovering_.store(false, std::memory_order_release);
        {
            std::scoped_lock lock(device_id_mutex_);
            current_device_id_.clear();
        }
    }

    void CleanupComApartment() noexcept {
        if (com_initialized_here_) {
            CoUninitialize();
            com_initialized_here_ = false;
        }
    }

    void CloseHandles() noexcept {
        for (auto* const handle : {audio_event_, shutdown_event_, reopen_event_}) {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
        audio_event_ = nullptr;
        shutdown_event_ = nullptr;
        reopen_event_ = nullptr;
    }

    WasapiOutputOptions options_;
    airplaywin::audio::AudioFormat format_{};
    std::unique_ptr<airplaywin::audio::AudioRingBuffer> ring_;
    std::unique_ptr<airplaywin::audio::AudioTransitionGuard> guard_;
    std::vector<float> render_scratch_{};
    airplaywin::audio::AudioDiagnostics diagnostics_{};

    HANDLE audio_event_{nullptr};
    HANDLE shutdown_event_{nullptr};
    HANDLE reopen_event_{nullptr};
    std::thread worker_{};
    mutable std::mutex control_mutex_;
    mutable std::mutex device_id_mutex_;

    ComPtr<IMMDeviceEnumerator> device_enumerator_;
    ComPtr<IMMDevice> endpoint_device_;
    ComPtr<IAudioClient> audio_client_;
    ComPtr<IAudioRenderClient> render_client_;
    ComPtr<NotificationClient> notification_client_;
    UINT32 endpoint_buffer_frames_{0U};
    std::uint32_t target_queue_frames_{0U};
    bool notification_registered_{false};
    bool com_initialized_here_{false};
    bool physical_client_started_{false};

    std::wstring current_device_id_;
    std::atomic<bool> opened_{false};
    std::atomic<bool> requested_playing_{false};
    std::atomic<std::uint64_t> current_epoch_{1U};
    std::atomic<std::uint32_t> current_buffer_depth_frames_{0U};
    std::atomic<std::uint64_t> output_latency_microseconds_{0U};
    std::atomic<std::uint64_t> software_queue_latency_microseconds_{0U};
    std::atomic<std::uint64_t> endpoint_padding_latency_microseconds_{0U};
    std::atomic<std::uint32_t> published_endpoint_buffer_frames_{0U};
    std::atomic<std::uint32_t> engine_period_frames_{0U};
    std::atomic<std::uint32_t> published_target_queue_frames_{0U};
    std::atomic<std::uint64_t> engine_latency_microseconds_{0U};
    std::atomic<AudioOutputMode> active_output_mode_{AudioOutputMode::Shared};
    std::atomic<AudioClientPath> audio_client_path_{AudioClientPath::Legacy};
    std::atomic<AudioEndpointSampleFormat> endpoint_sample_format_{
        AudioEndpointSampleFormat::Float32};
    std::atomic<bool> low_latency_active_{false};
    std::atomic<bool> output_mode_fallback_{false};
    std::atomic<HRESULT> output_mode_fallback_error_{S_OK};
    std::atomic<bool> force_shared_fallback_{false};
    std::atomic<HRESULT> force_shared_fallback_error_{S_OK};
    std::atomic<std::uint64_t> render_wakeup_count_{0U};
    std::atomic<std::uint64_t> exclusive_start_timeouts_{0U};
    std::atomic<std::uint64_t> device_switch_events_{0U};
    std::atomic<std::uint64_t> device_recovery_attempts_{0U};
    std::atomic<std::uint64_t> device_recovery_successes_{0U};
    std::atomic<std::uint64_t> device_recovery_failures_{0U};
    std::atomic<bool> output_recovering_{false};
    std::atomic<HRESULT> last_error_{S_OK};
};

WasapiAudioOutput::WasapiAudioOutput(WasapiOutputOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

WasapiAudioOutput::~WasapiAudioOutput() = default;

bool WasapiAudioOutput::Open(const airplaywin::audio::AudioFormat& format) {
    return impl_->Open(format);
}

bool WasapiAudioOutput::Start() {
    return impl_->Start();
}

bool WasapiAudioOutput::Write(const airplaywin::audio::AudioBuffer& buffer) {
    return impl_->Write(buffer);
}

void WasapiAudioOutput::Pause() noexcept {
    impl_->Pause();
}

void WasapiAudioOutput::Resume() noexcept {
    impl_->Resume();
}

void WasapiAudioOutput::Stop() noexcept {
    impl_->Stop();
}

void WasapiAudioOutput::Flush() noexcept {
    impl_->Flush();
}

void WasapiAudioOutput::BeginEpoch(const std::uint64_t epoch_id,
                                   const airplaywin::audio::AudioTransition transition) noexcept {
    impl_->BeginEpoch(epoch_id, transition);
}

void WasapiAudioOutput::SetVolume(const float linear_gain) noexcept {
    impl_->SetVolume(linear_gain);
}

void WasapiAudioOutput::Close() noexcept {
    impl_->Close();
}

airplaywin::audio::AudioDiagnosticsSnapshot WasapiAudioOutput::Diagnostics() const {
    return impl_->Diagnostics();
}

}  // namespace airplaywin::windows::audio
