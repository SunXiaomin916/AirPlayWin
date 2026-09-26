#include "platform/windows/audio/WindowsAlacDecoder.h"

#include <windows.h>
#include <bcrypt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace airplaywin::windows::audio {
namespace {

using Microsoft::WRL::ComPtr;

constexpr CLSID kAlacDecoderClsid{
    0xC0CD7D12U, 0x31FCU, 0x4BBCU, {0xB3U, 0x63U, 0x73U, 0x22U, 0xEEU, 0x3EU, 0x18U, 0x79U}};
constexpr std::size_t kMaximumEncodedPacketBytes = 4U * 1'024U;

[[nodiscard]] bool NtSucceeded(const NTSTATUS status) noexcept {
    return status >= 0;
}

class ComApartment final {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

    ~ComApartment() {
        if (result_ == S_OK || result_ == S_FALSE) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool Available() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_{E_FAIL};
};

[[nodiscard]] bool EnsureComApartment() noexcept {
    thread_local ComApartment apartment;
    return apartment.Available();
}

[[nodiscard]] std::uint8_t CookieBitDepth(
    const airplaywin::audio::EncodedAudioFormat& format) noexcept {
    return format.codec_config_size > 5U
               ? std::to_integer<std::uint8_t>(format.codec_config[5U])
               : 0U;
}

}  // namespace

class WindowsAlacDecoder::Impl final {
public:
    ~Impl() {
        Shutdown();
    }

    [[nodiscard]] bool Configure(const airplaywin::audio::EncodedAudioFormat& format) {
        Shutdown();
        if (!format.IsValid() || format.codec != airplaywin::audio::AudioCodec::AppleLossless ||
            !EnsureComApartment()) {
            return false;
        }
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            return false;
        }
        media_foundation_started_ = true;

        if (FAILED(CoCreateInstance(kAlacDecoderClsid, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&transform_)))) {
            Shutdown();
            return false;
        }

        ComPtr<IMFMediaType> input_type;
        if (FAILED(MFCreateMediaType(&input_type)) ||
            FAILED(input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
            FAILED(input_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_ALAC)) ||
            FAILED(input_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.channel_count)) ||
            FAILED(input_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format.sample_rate)) ||
            FAILED(input_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, CookieBitDepth(format))) ||
            FAILED(input_type->SetBlob(
                MF_MT_USER_DATA,
                reinterpret_cast<const UINT8*>(format.codec_config.data()),
                format.codec_config_size)) ||
            FAILED(transform_->SetInputType(0U, input_type.Get(), 0U))) {
            Shutdown();
            return false;
        }

        if (!SetOutputType(format, MFAudioFormat_Float, 32U, true) &&
            !SetOutputType(format, MFAudioFormat_PCM, 16U, false)) {
            Shutdown();
            return false;
        }

        MFT_OUTPUT_STREAM_INFO output_info{};
        if (FAILED(transform_->GetOutputStreamInfo(0U, &output_info))) {
            Shutdown();
            return false;
        }
        // The inbox ALAC decoder advertises CAN_PROVIDE_SAMPLES rather than the
        // stricter PROVIDES_SAMPLES flag.  Passing a reusable caller sample in
        // that mode makes the decoder append a new PCM buffer on every call,
        // so the apparent output grows by one packet until validation fails.
        // Let the transform own each output sample whenever either capability
        // is present, then release that sample after copying it to our fixed
        // decode storage.
        constexpr DWORD kTransformAllocatesSamples =
            MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES;
        mft_allocates_output_samples_ =
            (output_info.dwFlags & kTransformAllocatesSamples) != 0U;
        const auto calculated_output_bytes =
            static_cast<std::size_t>(format.nominal_frames_per_packet) *
            format.channel_count * (float_output_ ? sizeof(float) : sizeof(std::int16_t));
        const auto output_bytes = (std::max)(calculated_output_bytes,
                                             static_cast<std::size_t>(output_info.cbSize));
        if (output_bytes == 0U || output_bytes > (std::numeric_limits<DWORD>::max)() ||
            FAILED(MFCreateSample(&input_sample_)) ||
            FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(kMaximumEncodedPacketBytes),
                                        &input_buffer_)) ||
            FAILED(input_sample_->AddBuffer(input_buffer_.Get()))) {
            Shutdown();
            return false;
        }
        if (!mft_allocates_output_samples_ &&
            (FAILED(MFCreateSample(&output_sample_)) ||
             FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(output_bytes), &output_buffer_)) ||
             FAILED(output_sample_->AddBuffer(output_buffer_.Get())))) {
            Shutdown();
            return false;
        }

        if (format.encrypted && !ConfigureAes(format)) {
            Shutdown();
            return false;
        }
        encrypted_storage_.resize(kMaximumEncodedPacketBytes);
        format_ = format;
        configured_ = true;
        static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0U));
        static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0U));
        return true;
    }

    [[nodiscard]] airplaywin::audio::DecodeResult Decode(
        const airplaywin::audio::EncodedAudioFrame& frame,
        const std::span<float> output) noexcept {
        using airplaywin::audio::DecodeResult;
        using airplaywin::audio::DecodeStatus;
        if (!configured_ || !transform_ || !EnsureComApartment()) {
            return {.status = DecodeStatus::NotConfigured};
        }
        if (frame.payload.empty() || frame.payload.size() > kMaximumEncodedPacketBytes) {
            return {.status = DecodeStatus::MalformedPayload};
        }

        auto input = frame.payload;
        if (format_.encrypted) {
            std::ranges::copy(frame.payload, encrypted_storage_.begin());
            const auto encrypted_bytes = frame.payload.size() & ~std::size_t{15U};
            if (encrypted_bytes != 0U) {
                auto iv = format_.encryption_iv;
                ULONG clear_bytes = 0U;
                const auto decrypt_status = BCryptDecrypt(
                        aes_key_, reinterpret_cast<PUCHAR>(encrypted_storage_.data()),
                        static_cast<ULONG>(encrypted_bytes), nullptr,
                        reinterpret_cast<PUCHAR>(iv.data()), static_cast<ULONG>(iv.size()),
                        reinterpret_cast<PUCHAR>(encrypted_storage_.data()),
                        static_cast<ULONG>(encrypted_bytes), &clear_bytes, 0U);
                if (!NtSucceeded(decrypt_status) ||
                    clear_bytes != encrypted_bytes) {
                    return {
                        .status = DecodeStatus::MalformedPayload,
                        .failure_point = airplaywin::audio::DecodeFailurePoint::Decryption,
                        .platform_error = static_cast<std::uint32_t>(decrypt_status),
                    };
                }
            }
            input = std::span<const std::byte>{encrypted_storage_.data(), frame.payload.size()};
        }

        BYTE* input_data = nullptr;
        DWORD input_capacity = 0U;
        const auto input_lock_result =
            input_buffer_->Lock(&input_data, &input_capacity, nullptr);
        if (FAILED(input_lock_result)) {
            return {
                .status = DecodeStatus::MalformedPayload,
                .failure_point = airplaywin::audio::DecodeFailurePoint::InputBuffer,
                .platform_error = static_cast<std::uint32_t>(input_lock_result),
            };
        }
        if (input_capacity < input.size()) {
            static_cast<void>(input_buffer_->Unlock());
            return {
                .status = DecodeStatus::MalformedPayload,
                .failure_point = airplaywin::audio::DecodeFailurePoint::InputBuffer,
            };
        }
        std::memcpy(input_data, input.data(), input.size());
        static_cast<void>(input_buffer_->Unlock());
        const auto set_length_result =
            input_buffer_->SetCurrentLength(static_cast<DWORD>(input.size()));
        if (FAILED(set_length_result)) {
            return {
                .status = DecodeStatus::MalformedPayload,
                .failure_point = airplaywin::audio::DecodeFailurePoint::InputBuffer,
                .platform_error = static_cast<std::uint32_t>(set_length_result),
            };
        }
        constexpr std::uint64_t kMediaFoundationTicksPerSecond = 10'000'000U;
        const auto sample_time = static_cast<LONGLONG>(
            static_cast<std::uint64_t>(frame.rtp_timestamp) *
            kMediaFoundationTicksPerSecond / format_.sample_rate);
        const auto sample_duration = static_cast<LONGLONG>(
            static_cast<std::uint64_t>(format_.nominal_frames_per_packet) *
            kMediaFoundationTicksPerSecond / format_.sample_rate);
        const auto time_result = input_sample_->SetSampleTime(sample_time);
        const auto duration_result = input_sample_->SetSampleDuration(sample_duration);
        if (FAILED(time_result) || FAILED(duration_result)) {
            const auto failure = FAILED(time_result) ? time_result : duration_result;
            return {
                .status = DecodeStatus::MalformedPayload,
                .failure_point = airplaywin::audio::DecodeFailurePoint::DecoderInput,
                .platform_error = static_cast<std::uint32_t>(failure),
            };
        }
        const auto input_result = transform_->ProcessInput(0U, input_sample_.Get(), 0U);
        if (FAILED(input_result)) {
            return {
                .status = DecodeStatus::MalformedPayload,
                .failure_point = airplaywin::audio::DecodeFailurePoint::DecoderInput,
                .platform_error = static_cast<std::uint32_t>(input_result),
            };
        }

        if (output_sample_ && output_buffer_) {
            // Some audio MFTs append their result buffer to the supplied sample
            // instead of reusing its first buffer.  Rebuild the sample's buffer
            // list for every packet so ConvertToContiguousBuffer cannot combine
            // PCM from previous packets.  The memory buffer itself remains
            // preallocated and is reused.
            const auto remove_result = output_sample_->RemoveAllBuffers();
            const auto length_result = output_buffer_->SetCurrentLength(0U);
            const auto add_result = output_sample_->AddBuffer(output_buffer_.Get());
            if (FAILED(remove_result) || FAILED(length_result) || FAILED(add_result)) {
                const auto failure = FAILED(remove_result)
                                         ? remove_result
                                         : (FAILED(length_result) ? length_result : add_result);
                return {
                    .status = DecodeStatus::MalformedPayload,
                    .failure_point = airplaywin::audio::DecodeFailurePoint::OutputBuffer,
                    .platform_error = static_cast<std::uint32_t>(failure),
                };
            }
        }
        MFT_OUTPUT_DATA_BUFFER output_data{
            .dwStreamID = 0U,
            .pSample = mft_allocates_output_samples_ ? nullptr : output_sample_.Get(),
            .dwStatus = 0U,
            .pEvents = nullptr,
        };
        DWORD status = 0U;
        const auto process_result = transform_->ProcessOutput(0U, 1U, &output_data, &status);
        IMFCollection* const output_events = output_data.pEvents;
        output_data.pEvents = nullptr;
        if (output_events != nullptr) {
            output_events->Release();
        }
        if (FAILED(process_result)) {
            return {
                .status = DecodeStatus::MalformedPayload,
                .failure_point = airplaywin::audio::DecodeFailurePoint::DecoderOutput,
                .platform_error = static_cast<std::uint32_t>(process_result),
            };
        }

        ComPtr<IMFSample> provided_sample;
        IMFSample* const decoded_sample = output_data.pSample;
        if (decoded_sample == nullptr) {
            return {
                .status = DecodeStatus::MalformedPayload,
                .failure_point = airplaywin::audio::DecodeFailurePoint::OutputValidation,
                .platform_error = 1U,
            };
        }
        if (decoded_sample != output_sample_.Get()) {
            provided_sample.Attach(decoded_sample);
        }
        ComPtr<IMFMediaBuffer> contiguous;
        BYTE* decoded = nullptr;
        DWORD decoded_bytes = 0U;
        const auto contiguous_result =
            decoded_sample->ConvertToContiguousBuffer(&contiguous);
        if (FAILED(contiguous_result)) {
            return {
                .status = DecodeStatus::MalformedPayload,
                .failure_point = airplaywin::audio::DecodeFailurePoint::OutputBuffer,
                .platform_error = static_cast<std::uint32_t>(contiguous_result),
            };
        }
        const auto output_lock_result =
            contiguous->Lock(&decoded, nullptr, &decoded_bytes);
        if (FAILED(output_lock_result)) {
            return {
                .status = DecodeStatus::MalformedPayload,
                .failure_point = airplaywin::audio::DecodeFailurePoint::OutputBuffer,
                .platform_error = static_cast<std::uint32_t>(output_lock_result),
            };
        }
        const auto bytes_per_sample = float_output_ ? sizeof(float) : sizeof(std::int16_t);
        const auto sample_count = decoded_bytes / bytes_per_sample;
        const auto valid = decoded_bytes != 0U && decoded_bytes % bytes_per_sample == 0U &&
                           sample_count % format_.channel_count == 0U &&
                           sample_count <= output.size();
        if (valid && float_output_) {
            std::memcpy(output.data(), decoded, decoded_bytes);
        } else if (valid) {
            constexpr float kScale = 1.0F / 32'768.0F;
            for (std::size_t index = 0U; index < sample_count; ++index) {
                std::int16_t value = 0;
                std::memcpy(&value, decoded + index * sizeof(value), sizeof(value));
                output[index] = static_cast<float>(value) * kScale;
            }
        }
        static_cast<void>(contiguous->Unlock());
        if (!valid) {
            const std::uint32_t validation_error =
                decoded_bytes == 0U
                    ? 1U
                    : (decoded_bytes % bytes_per_sample != 0U
                           ? 2U
                           : (sample_count % format_.channel_count != 0U ? 3U : 4U));
            return {
                .status = sample_count > output.size() ? DecodeStatus::OutputTooSmall
                                                       : DecodeStatus::MalformedPayload,
                .failure_point = airplaywin::audio::DecodeFailurePoint::OutputValidation,
                .platform_error = validation_error,
            };
        }
        return {
            .status = DecodeStatus::Ok,
            .frame_count = static_cast<std::uint32_t>(sample_count / format_.channel_count),
        };
    }

    [[nodiscard]] airplaywin::audio::DecodeResult ConcealLoss(
        const std::uint32_t frame_count,
        const std::span<float> output) noexcept {
        using airplaywin::audio::DecodeResult;
        using airplaywin::audio::DecodeStatus;
        if (!configured_) {
            return {.status = DecodeStatus::NotConfigured};
        }
        const auto sample_count = static_cast<std::size_t>(frame_count) * format_.channel_count;
        if (frame_count == 0U || sample_count > output.size()) {
            return {.status = DecodeStatus::OutputTooSmall};
        }
        std::ranges::fill(output.first(sample_count), 0.0F);
        return {.status = DecodeStatus::Ok, .frame_count = frame_count};
    }

    void Reset() noexcept {
        if (!configured_ || !transform_) {
            return;
        }
        static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0U));
        static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0U));
    }

private:
    [[nodiscard]] bool SetOutputType(const airplaywin::audio::EncodedAudioFormat& format,
                                     const GUID& subtype,
                                     const UINT32 bits_per_sample,
                                     const bool use_float) {
        ComPtr<IMFMediaType> output_type;
        const auto block_alignment =
            static_cast<UINT32>(format.channel_count) * bits_per_sample / 8U;
        if (FAILED(MFCreateMediaType(&output_type)) ||
            FAILED(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
            FAILED(output_type->SetGUID(MF_MT_SUBTYPE, subtype)) ||
            FAILED(output_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.channel_count)) ||
            FAILED(output_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                          format.sample_rate)) ||
            FAILED(output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits_per_sample)) ||
            FAILED(output_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_alignment)) ||
            FAILED(output_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                          block_alignment * format.sample_rate)) ||
            FAILED(output_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE)) ||
            FAILED(transform_->SetOutputType(0U, output_type.Get(), 0U))) {
            return false;
        }
        float_output_ = use_float;
        return true;
    }

    [[nodiscard]] bool ConfigureAes(
        const airplaywin::audio::EncodedAudioFormat& format) {
        if (!NtSucceeded(BCryptOpenAlgorithmProvider(
                &aes_algorithm_, BCRYPT_AES_ALGORITHM, nullptr, 0U)) ||
            !NtSucceeded(BCryptSetProperty(
                aes_algorithm_, BCRYPT_CHAINING_MODE,
                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_CBC)), 0U))) {
            return false;
        }
        ULONG object_bytes = 0U;
        ULONG returned_bytes = 0U;
        if (!NtSucceeded(BCryptGetProperty(
                aes_algorithm_, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
                &returned_bytes, 0U)) ||
            returned_bytes != sizeof(object_bytes) || object_bytes == 0U) {
            return false;
        }
        aes_key_object_.resize(object_bytes);
        auto* const key_data = reinterpret_cast<PUCHAR>(
            const_cast<std::byte*>(format.encryption_key.data()));
        return NtSucceeded(BCryptGenerateSymmetricKey(
            aes_algorithm_, &aes_key_, aes_key_object_.data(), object_bytes, key_data,
            static_cast<ULONG>(format.encryption_key.size()), 0U));
    }

    void Shutdown() noexcept {
        configured_ = false;
        if (transform_) {
            static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0U));
            static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0U));
        }
        output_buffer_.Reset();
        output_sample_.Reset();
        input_buffer_.Reset();
        input_sample_.Reset();
        transform_.Reset();
        if (aes_key_ != nullptr) {
            static_cast<void>(BCryptDestroyKey(aes_key_));
            aes_key_ = nullptr;
        }
        if (aes_algorithm_ != nullptr) {
            static_cast<void>(BCryptCloseAlgorithmProvider(aes_algorithm_, 0U));
            aes_algorithm_ = nullptr;
        }
        if (!aes_key_object_.empty()) {
            SecureZeroMemory(aes_key_object_.data(), aes_key_object_.size());
            aes_key_object_.clear();
        }
        if (!encrypted_storage_.empty()) {
            SecureZeroMemory(encrypted_storage_.data(), encrypted_storage_.size());
            encrypted_storage_.clear();
        }
        SecureZeroMemory(format_.encryption_key.data(), format_.encryption_key.size());
        format_ = {};
        mft_allocates_output_samples_ = false;
        if (media_foundation_started_) {
            static_cast<void>(MFShutdown());
            media_foundation_started_ = false;
        }
    }

    airplaywin::audio::EncodedAudioFormat format_{};
    ComPtr<IMFTransform> transform_;
    ComPtr<IMFSample> input_sample_;
    ComPtr<IMFMediaBuffer> input_buffer_;
    ComPtr<IMFSample> output_sample_;
    ComPtr<IMFMediaBuffer> output_buffer_;
    BCRYPT_ALG_HANDLE aes_algorithm_{nullptr};
    BCRYPT_KEY_HANDLE aes_key_{nullptr};
    std::vector<UCHAR> aes_key_object_;
    std::vector<std::byte> encrypted_storage_;
    bool media_foundation_started_{false};
    bool configured_{false};
    bool float_output_{false};
    bool mft_allocates_output_samples_{false};
};

WindowsAlacDecoder::WindowsAlacDecoder() : impl_(std::make_unique<Impl>()) {}

WindowsAlacDecoder::~WindowsAlacDecoder() = default;

bool WindowsAlacDecoder::Configure(const airplaywin::audio::EncodedAudioFormat& format) {
    return impl_ != nullptr && impl_->Configure(format);
}

airplaywin::audio::DecodeResult WindowsAlacDecoder::Decode(
    const airplaywin::audio::EncodedAudioFrame& frame,
    const std::span<float> output) noexcept {
    return impl_ != nullptr ? impl_->Decode(frame, output)
                            : airplaywin::audio::DecodeResult{};
}

airplaywin::audio::DecodeResult WindowsAlacDecoder::ConcealLoss(
    const std::uint32_t frame_count,
    const std::span<float> output) noexcept {
    return impl_ != nullptr
               ? impl_->ConcealLoss(frame_count, output)
               : airplaywin::audio::DecodeResult{};
}

void WindowsAlacDecoder::Reset() noexcept {
    if (impl_ != nullptr) {
        impl_->Reset();
    }
}

}  // namespace airplaywin::windows::audio
