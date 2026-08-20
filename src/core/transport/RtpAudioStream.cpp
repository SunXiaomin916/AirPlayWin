#include "core/transport/RtpAudioStream.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "core/transport/RtpPacket.h"

namespace airplaywin::transport {

RtpAudioStream::RtpAudioStream(RtpAudioStreamConfig config,
                               std::unique_ptr<audio::IAudioDecoder> decoder,
                               audio::IAudioFrameSink& sink)
    : config_(config),
      decoder_(std::move(decoder)),
      sink_(sink),
      jitter_buffer_(config.jitter_buffer) {
    if (config_.connection_id == 0U || !config_.format.IsValid() || !decoder_) {
        throw std::invalid_argument("invalid RTP audio stream configuration");
    }
    const auto max_payload_frames =
        kMaxRtpPayloadBytes / (2U * static_cast<std::size_t>(config_.format.channel_count));
    const auto max_frames = std::max<std::size_t>(config_.format.nominal_frames_per_packet,
                                                  max_payload_frames);
    decode_storage_.resize(max_frames * config_.format.channel_count);
}

RtpAudioStream::~RtpAudioStream() {
    Stop();
}

bool RtpAudioStream::Start() {
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }
    if (!decoder_->Configure(config_.format)) {
        last_error_.store(1U, std::memory_order_release);
        return false;
    }
    if (!sink_.Configure(config_.format.DecodedFormat())) {
        last_error_.store(2U, std::memory_order_release);
        return false;
    }
    running_.store(true, std::memory_order_release);
    configured_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread{[this] { WorkerLoop(); }};
    } catch (...) {
        configured_.store(false, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        sink_.Stop();
        last_error_.store(3U, std::memory_order_release);
        return false;
    }
    return true;
}

bool RtpAudioStream::Record(const AudioTimelineAnchor& anchor) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    std::scoped_lock processing_lock{processing_mutex_};
    ResetPacketTimeline(anchor);
    if (!sink_.Start()) {
        last_error_.store(4U, std::memory_order_release);
        return false;
    }
    recording_.store(true, std::memory_order_release);
    wake_.notify_all();
    return true;
}

void RtpAudioStream::Pause() noexcept {
    recording_.store(false, std::memory_order_release);
    std::scoped_lock processing_lock{processing_mutex_};
    sink_.Pause();
}

void RtpAudioStream::Resume(const AudioTimelineAnchor& anchor) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::scoped_lock processing_lock{processing_mutex_};
        if (!anchor.IsEmpty()) {
            ResetPacketTimeline(anchor);
            sink_.Flush();
        }
        sink_.Resume();
    }
    recording_.store(true, std::memory_order_release);
    wake_.notify_all();
}

void RtpAudioStream::Flush(const AudioTimelineAnchor& anchor) noexcept {
    std::scoped_lock processing_lock{processing_mutex_};
    ResetPacketTimeline(anchor);
    sink_.Flush();
}

void RtpAudioStream::SetVolume(const float linear_gain) noexcept {
    std::scoped_lock processing_lock{processing_mutex_};
    sink_.SetVolume(linear_gain);
}

void RtpAudioStream::Stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    recording_.store(false, std::memory_order_release);
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::scoped_lock processing_lock{processing_mutex_};
    sink_.Stop();
    ResetPacketTimeline({});
    configured_.store(false, std::memory_order_release);
}

void RtpAudioStream::OnDatagram(const std::span<const std::byte> datagram,
                                const DatagramEndpoint& source,
                                const std::int64_t arrival_time_nanoseconds) noexcept {
    datagrams_received_.fetch_add(1U, std::memory_order_relaxed);
    datagram_bytes_.fetch_add(datagram.size(), std::memory_order_relaxed);
    if (config_.allowed_peer_ipv4_network_order != 0U &&
        source.ipv4_address_network_order != config_.allowed_peer_ipv4_network_order) {
        unexpected_source_packets_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    constexpr std::uint8_t kRetransmittedPayloadType = 0x56U;
    const bool is_retransmitted =
        datagram.size() >= 2U &&
        (std::to_integer<std::uint8_t>(datagram[1U]) & 0x7FU) ==
            kRetransmittedPayloadType;
    const auto rtp_datagram =
        is_retransmitted && datagram.size() >= 4U ? datagram.subspan(4U) : datagram;
    const auto parsed = ParseRtpPacket(rtp_datagram);
    if (!parsed.has_value()) {
        invalid_rtp_packets_.fetch_add(1U, std::memory_order_relaxed);
        last_error_.store(5U, std::memory_order_relaxed);
        return;
    }
    if (parsed->payload_type != config_.format.payload_type) {
        unexpected_payload_packets_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (is_retransmitted) {
        retransmitted_packets_.fetch_add(1U, std::memory_order_relaxed);
    }
    constexpr std::uint64_t kInitialized = std::uint64_t{1U} << 32U;
    std::uint64_t expected_source = stream_source_.load(std::memory_order_acquire);
    if (expected_source == 0U) {
        const auto desired_source = kInitialized | parsed->ssrc;
        static_cast<void>(stream_source_.compare_exchange_strong(
            expected_source, desired_source, std::memory_order_acq_rel));
        expected_source = stream_source_.load(std::memory_order_acquire);
    }
    const auto expected_ssrc = static_cast<std::uint32_t>(expected_source);
    if (parsed->ssrc != expected_ssrc) {
        unexpected_source_packets_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    std::scoped_lock timeline_lock{timeline_mutex_};
    if (!IsAtOrAfterTimeline(*parsed)) {
        timeline_rejected_packets_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (jitter_buffer_.Insert(*parsed, arrival_time_nanoseconds)) {
        wake_.notify_one();
    }
}

AudioTransportDiagnostics RtpAudioStream::Diagnostics() const noexcept {
    return AudioTransportDiagnostics{
        .configured = configured_.load(std::memory_order_acquire),
        .recording = recording_.load(std::memory_order_acquire),
        .connection_id = config_.connection_id,
        .datagrams_received = datagrams_received_.load(std::memory_order_relaxed),
        .datagram_bytes = datagram_bytes_.load(std::memory_order_relaxed),
        .invalid_rtp_packets = invalid_rtp_packets_.load(std::memory_order_relaxed),
        .unexpected_source_packets = unexpected_source_packets_.load(std::memory_order_relaxed),
        .unexpected_payload_packets =
            unexpected_payload_packets_.load(std::memory_order_relaxed),
        .retransmitted_packets = retransmitted_packets_.load(std::memory_order_relaxed),
        .timeline_rejected_packets =
            timeline_rejected_packets_.load(std::memory_order_relaxed),
        .timeline_resets = timeline_resets_.load(std::memory_order_relaxed),
        .has_sequence_anchor =
            (sequence_anchor_.load(std::memory_order_acquire) & 0x1'0000U) != 0U,
        .has_timestamp_anchor =
            (timestamp_anchor_.load(std::memory_order_acquire) &
             (std::uint64_t{1U} << 32U)) != 0U,
        .sequence_anchor = static_cast<std::uint16_t>(
            sequence_anchor_.load(std::memory_order_relaxed)),
        .timestamp_anchor = static_cast<std::uint32_t>(
            timestamp_anchor_.load(std::memory_order_relaxed)),
        .decoded_packets = decoded_packets_.load(std::memory_order_relaxed),
        .decoded_frames = decoded_frames_.load(std::memory_order_relaxed),
        .concealed_packets = concealed_packets_.load(std::memory_order_relaxed),
        .concealed_frames = concealed_frames_.load(std::memory_order_relaxed),
        .decoder_errors = decoder_errors_.load(std::memory_order_relaxed),
        .sink_backpressure_events = sink_backpressure_events_.load(std::memory_order_relaxed),
        .jitter_buffer = jitter_buffer_.Diagnostics(),
        .last_error = last_error_.load(std::memory_order_relaxed),
    };
}

void RtpAudioStream::ResetPacketTimeline(const AudioTimelineAnchor& anchor) noexcept {
    std::scoped_lock timeline_lock{timeline_mutex_};
    jitter_buffer_.Flush();
    decoder_->Reset();
    have_decoded_frame_ = false;
    last_rtp_timestamp_ = 0U;
    last_frame_count_ = 0U;
    sequence_anchor_.store(anchor.sequence_number.has_value()
                               ? 0x1'0000U | *anchor.sequence_number
                               : 0U,
                           std::memory_order_release);
    timestamp_anchor_.store(anchor.rtp_timestamp.has_value()
                                ? (std::uint64_t{1U} << 32U) | *anchor.rtp_timestamp
                                : 0U,
                            std::memory_order_release);
    timeline_resets_.fetch_add(1U, std::memory_order_relaxed);
}

bool RtpAudioStream::IsAtOrAfterTimeline(const RtpPacketView& packet) const noexcept {
    const auto sequence_anchor = sequence_anchor_.load(std::memory_order_acquire);
    if ((sequence_anchor & 0x1'0000U) != 0U &&
        static_cast<std::int16_t>(packet.sequence_number -
                                  static_cast<std::uint16_t>(sequence_anchor)) < 0) {
        return false;
    }
    const auto timestamp_anchor = timestamp_anchor_.load(std::memory_order_acquire);
    return (timestamp_anchor & (std::uint64_t{1U} << 32U)) == 0U ||
           static_cast<std::int32_t>(packet.timestamp -
                                     static_cast<std::uint32_t>(timestamp_anchor)) >= 0;
}

void RtpAudioStream::WorkerLoop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock state_lock{state_mutex_};
            wake_.wait_for(state_lock, std::chrono::milliseconds{2}, [this] {
                return !running_.load(std::memory_order_acquire) ||
                       recording_.load(std::memory_order_acquire);
            });
        }
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        if (recording_.load(std::memory_order_acquire)) {
            ProcessAvailablePackets();
        }
    }
}

void RtpAudioStream::ProcessAvailablePackets() noexcept {
    for (;;) {
        if (!running_.load(std::memory_order_acquire) ||
            !recording_.load(std::memory_order_acquire)) {
            return;
        }
        std::scoped_lock processing_lock{processing_mutex_};
        BufferedRtpPacket packet;
        const auto kind = jitter_buffer_.Pop(packet);
        if (kind == JitterPopKind::Waiting) {
            return;
        }

        audio::DecodeResult result;
        std::uint32_t rtp_timestamp = packet.timestamp;
        if (kind == JitterPopKind::Packet) {
            result = decoder_->Decode(
                audio::EncodedAudioFrame{.payload = packet.Payload(),
                                         .extended_sequence_number =
                                             packet.extended_sequence_number,
                                         .rtp_timestamp = packet.timestamp,
                                         .marker = packet.marker},
                decode_storage_);
            if (result.status == audio::DecodeStatus::Ok) {
                decoded_packets_.fetch_add(1U, std::memory_order_relaxed);
                decoded_frames_.fetch_add(result.frame_count, std::memory_order_relaxed);
            }
        } else {
            const auto frames = have_decoded_frame_ ? last_frame_count_
                                                    : config_.format.nominal_frames_per_packet;
            result = decoder_->ConcealLoss(frames, decode_storage_);
            rtp_timestamp = have_decoded_frame_ ? last_rtp_timestamp_ + last_frame_count_ : 0U;
            if (result.status == audio::DecodeStatus::Ok) {
                concealed_packets_.fetch_add(1U, std::memory_order_relaxed);
                concealed_frames_.fetch_add(result.frame_count, std::memory_order_relaxed);
            }
        }
        if (result.status != audio::DecodeStatus::Ok || result.frame_count == 0U) {
            decoder_errors_.fetch_add(1U, std::memory_order_relaxed);
            last_error_.store(6U, std::memory_order_relaxed);
            continue;
        }
        const auto samples = static_cast<std::size_t>(result.frame_count) *
                             config_.format.channel_count;
        const audio::DecodedAudioFrameView decoded{
            .interleaved_samples = std::span<const float>{decode_storage_.data(), samples},
            .frame_count = result.frame_count,
            .rtp_timestamp = rtp_timestamp,
            .extended_sequence_number = packet.extended_sequence_number,
            .concealed = kind == JitterPopKind::Missing,
        };
        if (!SubmitWithBackpressure(decoded)) {
            last_error_.store(7U, std::memory_order_relaxed);
        }
        have_decoded_frame_ = true;
        last_rtp_timestamp_ = rtp_timestamp;
        last_frame_count_ = result.frame_count;
    }
}

bool RtpAudioStream::SubmitWithBackpressure(
    const audio::DecodedAudioFrameView& frame) noexcept {
    if (sink_.Submit(frame)) {
        return true;
    }
    sink_backpressure_events_.fetch_add(1U, std::memory_order_relaxed);
    for (std::uint32_t retry = 0U; retry < 100U; ++retry) {
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        if (sink_.Submit(frame)) {
            return true;
        }
    }
    return false;
}

}  // namespace airplaywin::transport
