#include "core/transport/RtpAudioStream.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

#include "core/transport/RtpPacket.h"

namespace airplaywin::transport {
namespace {

[[nodiscard]] std::uint64_t SaturatingAdd(const std::uint64_t left,
                                          const std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

}  // namespace

RtpAudioStream::RtpAudioStream(RtpAudioStreamConfig config,
                               std::unique_ptr<audio::IAudioDecoder> decoder,
                               audio::IAudioFrameSink& sink,
                               std::unique_ptr<timing::ITimingEngine> timing_engine)
    : config_(config),
      decoder_(std::move(decoder)),
      sink_(sink),
      timing_engine_(std::move(timing_engine)),
      jitter_buffer_(config.jitter_buffer) {
    if (config_.connection_id == 0U || !config_.format.IsValid() || !decoder_) {
        throw std::invalid_argument("invalid RTP audio stream configuration");
    }
    const auto max_payload_frames =
        kMaxRtpPayloadBytes / (2U * static_cast<std::size_t>(config_.format.channel_count));
    const auto max_frames = std::max<std::size_t>(config_.format.nominal_frames_per_packet,
                                                  max_payload_frames);
    decode_storage_.resize(max_frames * config_.format.channel_count);
    const auto maximum_resampled_frames = max_frames + max_frames / 50U + 2U;
    resample_storage_.resize(maximum_resampled_frames * config_.format.channel_count);
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
        } else if (timing_engine_) {
            // Without a remote wall clock, a local buffered anchor cannot survive an
            // arbitrary RTSP pause. Re-lock on the first resumed packet.
            timing_engine_->Reset(std::nullopt);
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
    const auto processing_started = std::chrono::steady_clock::now();
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
    const auto inserted = jitter_buffer_.Insert(*parsed, arrival_time_nanoseconds);
    const auto processing_nanoseconds = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - processing_started)
               .count());
    RecordPacketCost(std::max<std::uint64_t>(
        1U, static_cast<std::uint64_t>((processing_nanoseconds + 999LL) / 1'000LL)));
    if (inserted) {
        wake_.notify_one();
    }
}

AudioTransportDiagnostics RtpAudioStream::Diagnostics() const noexcept {
    const auto jitter = jitter_buffer_.Diagnostics();
    const auto timing = timing_engine_ ? timing_engine_->Diagnostics()
                                       : timing::TimingDiagnostics{};
    const auto feedback = sink_.Feedback();
    const auto fallback_packet_duration =
        static_cast<std::uint64_t>(config_.format.nominal_frames_per_packet) * 1'000'000U /
        config_.format.sample_rate;
    const auto packet_duration = jitter.observed_packet_duration_microseconds != 0U
                                     ? jitter.observed_packet_duration_microseconds
                                     : fallback_packet_duration;
    const auto reserve_packets = jitter.target_packets > 0U ? jitter.target_packets - 1U : 0U;
    const auto jitter_reserve = static_cast<std::uint64_t>(reserve_packets) * packet_duration;
    const auto scheduled_reserve =
        std::max(jitter_reserve,
                 timing.enabled ? timing.target_buffer_microseconds : std::uint64_t{0U});
    const auto decode_average =
        decode_processing_average_microseconds_.load(std::memory_order_relaxed);
    const auto packet_average =
        packet_processing_average_microseconds_.load(std::memory_order_relaxed);
    auto receiver_added = SaturatingAdd(scheduled_reserve, packet_average);
    receiver_added = SaturatingAdd(receiver_added, decode_average);
    receiver_added = SaturatingAdd(receiver_added, feedback.output_latency_microseconds);
    return AudioTransportDiagnostics{
        .configured = configured_.load(std::memory_order_acquire),
        .recording = recording_.load(std::memory_order_acquire),
        .connection_id = config_.connection_id,
        .protocol_latency_frames = config_.protocol_latency_frames,
        .protocol_latency_microseconds =
            static_cast<std::uint64_t>(config_.protocol_latency_frames) * 1'000'000U /
            config_.format.sample_rate,
        .datagrams_received = datagrams_received_.load(std::memory_order_relaxed),
        .datagram_bytes = datagram_bytes_.load(std::memory_order_relaxed),
        .invalid_rtp_packets = invalid_rtp_packets_.load(std::memory_order_relaxed),
        .unexpected_source_packets = unexpected_source_packets_.load(std::memory_order_relaxed),
        .unexpected_payload_packets =
            unexpected_payload_packets_.load(std::memory_order_relaxed),
        .retransmitted_packets = retransmitted_packets_.load(std::memory_order_relaxed),
        .resend_requests_sent = resend_requests_sent_.load(std::memory_order_relaxed),
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
        .resampled_input_frames =
            resampled_input_frames_.load(std::memory_order_relaxed),
        .resampled_output_frames =
            resampled_output_frames_.load(std::memory_order_relaxed),
        .drift_inserted_frames = drift_inserted_frames_.load(std::memory_order_relaxed),
        .drift_dropped_frames = drift_dropped_frames_.load(std::memory_order_relaxed),
        .timing_hard_resync_requests =
            timing_hard_resync_requests_.load(std::memory_order_relaxed),
        .timing_unmapped_dropped_frames =
            timing_unmapped_dropped_frames_.load(std::memory_order_relaxed),
        .packet_processing_average_microseconds = packet_average,
        .packet_processing_maximum_microseconds =
            packet_processing_maximum_microseconds_.load(std::memory_order_relaxed),
        .decoder_errors = decoder_errors_.load(std::memory_order_relaxed),
        .last_decoder_failure_point =
            last_decoder_failure_point_.load(std::memory_order_relaxed),
        .last_decoder_platform_error =
            last_decoder_platform_error_.load(std::memory_order_relaxed),
        .decode_processing_average_microseconds = decode_average,
        .decode_processing_maximum_microseconds =
            decode_processing_maximum_microseconds_.load(std::memory_order_relaxed),
        .decode_budget_miss_count =
            decode_budget_miss_count_.load(std::memory_order_relaxed),
        .sink_backpressure_events = sink_backpressure_events_.load(std::memory_order_relaxed),
        .jitter_reserve_microseconds = jitter_reserve,
        .scheduled_reserve_microseconds = scheduled_reserve,
        .output_path_latency_microseconds = feedback.output_latency_microseconds,
        .receiver_added_latency_estimate_microseconds = receiver_added,
        .jitter_buffer = jitter,
        .timing = timing,
        .last_error = last_error_.load(std::memory_order_relaxed),
    };
}

void RtpAudioStream::ResetPacketTimeline(const AudioTimelineAnchor& anchor) noexcept {
    std::scoped_lock timeline_lock{timeline_mutex_};
    jitter_buffer_.Flush();
    decoder_->Reset();
    drift_resampler_.Reset();
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
    if (timing_engine_) {
        timing_engine_->Reset(
            anchor.rtp_timestamp.has_value()
                ? std::optional<std::uint64_t>{*anchor.rtp_timestamp}
                : std::optional<std::uint64_t>{});
    }
    jitter_buffer_.UpdateRuntimeConditions(
        {.timing_locked = !timing_engine_ || timing_engine_->Diagnostics().locked,
         .decode_margin_sufficient = false});
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

namespace {

[[nodiscard]] std::int64_t NowNanoseconds() noexcept {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

}  // namespace

void RtpAudioStream::WorkerLoop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock state_lock{state_mutex_};
            // Sleep while idle or while the buffer has nothing to deliver.
            // The predicate queries the jitter buffer directly so a packet
            // inserted between the check and the wait is never missed. The
            // 20 ms timeout only bounds the latency of a loss-gap deadline
            // expiry; waiting on the predicate keeps this thread at ~0% CPU
            // between packets.
            wake_.wait_for(state_lock, std::chrono::milliseconds{20}, [this] {
                if (!running_.load(std::memory_order_acquire) ||
                    !recording_.load(std::memory_order_acquire)) {
                    return false;
                }
                return jitter_buffer_.HasAvailable(NowNanoseconds());
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

void RtpAudioStream::DrainResendRequests() noexcept {
    std::uint16_t first_sequence = 0U;
    std::uint16_t count = 0U;
    while (jitter_buffer_.TakePendingResendRequest(first_sequence, count)) {
        if (config_.resend_requester) {
            config_.resend_requester(first_sequence, count);
            resend_requests_sent_.fetch_add(1U, std::memory_order_relaxed);
        }
    }
}

void RtpAudioStream::ProcessAvailablePackets() noexcept {
    for (;;) {
        if (!running_.load(std::memory_order_acquire) ||
            !recording_.load(std::memory_order_acquire)) {
            break;
        }
        // Ask the sender for retransmission of any loss gap the jitter
        // buffer opened, outside the processing lock so the (bounded) send
        // can never stall pause/stop/flush.
        DrainResendRequests();
        std::scoped_lock processing_lock{processing_mutex_};
        // Pause/stop can flip the atomic state after the loop-level check while
        // this worker is waiting for the transition mutex. Recheck under the
        // serialization boundary so a packet queued during pause is not popped
        // and rejected by the paused sink before resume can consume it.
        if (!running_.load(std::memory_order_acquire) ||
            !recording_.load(std::memory_order_acquire)) {
            break;
        }
        const auto feedback = sink_.Feedback();
        if (feedback.underrun_count > observed_sink_underruns_) {
            jitter_buffer_.ReportDownstreamUnderrun(feedback.underrun_count -
                                                    observed_sink_underruns_);
            observed_sink_underruns_ = feedback.underrun_count;
        }
        BufferedRtpPacket packet;
        const auto kind = jitter_buffer_.Pop(packet, NowNanoseconds());
        if (kind == JitterPopKind::Waiting) {
            break;
        }

        const auto decode_started = std::chrono::steady_clock::now();
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
            // A first-packet concealment before any decoded frame maps to the
            // timeline anchor when one exists, so timing engines can schedule
            // it instead of dropping the unmappable zero timestamp.
            const auto anchor = timestamp_anchor_.load(std::memory_order_relaxed);
            rtp_timestamp =
                have_decoded_frame_
                    ? last_rtp_timestamp_ + last_frame_count_
                    : ((anchor & (std::uint64_t{1U} << 32U)) != 0U
                           ? static_cast<std::uint32_t>(anchor)
                           : 0U);
            if (result.status == audio::DecodeStatus::Ok) {
                concealed_packets_.fetch_add(1U, std::memory_order_relaxed);
                concealed_frames_.fetch_add(result.frame_count, std::memory_order_relaxed);
            }
        }
        const auto decode_elapsed = std::chrono::steady_clock::now() - decode_started;
        const auto decode_nanoseconds = std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::nanoseconds>(decode_elapsed).count());
        const auto decode_microseconds = std::max<std::uint64_t>(
            1U, static_cast<std::uint64_t>((decode_nanoseconds + 999LL) / 1'000LL));
        const auto budget_frames = result.frame_count != 0U
                                       ? result.frame_count
                                       : config_.format.nominal_frames_per_packet;
        const auto packet_duration_microseconds =
            (static_cast<std::uint64_t>(budget_frames) * 1'000'000U +
             config_.format.sample_rate - 1U) /
            config_.format.sample_rate;
        const auto decode_margin_sufficient =
            result.status == audio::DecodeStatus::Ok && result.frame_count != 0U &&
            decode_microseconds <= std::max<std::uint64_t>(
                                       1U, packet_duration_microseconds / 2U);
        RecordDecodeCost(decode_microseconds, decode_margin_sufficient);
        const auto timing_locked = !timing_engine_ || timing_engine_->Diagnostics().locked;
        jitter_buffer_.UpdateRuntimeConditions(
            {.timing_locked = timing_locked,
             .decode_margin_sufficient = decode_margin_sufficient});
        if (result.status != audio::DecodeStatus::Ok || result.frame_count == 0U) {
            decoder_errors_.fetch_add(1U, std::memory_order_relaxed);
            last_decoder_failure_point_.store(result.failure_point,
                                              std::memory_order_relaxed);
            last_decoder_platform_error_.store(result.platform_error,
                                               std::memory_order_relaxed);
            last_error_.store(6U, std::memory_order_relaxed);
            continue;
        }
        auto submitted_frames = result.frame_count;
        auto submitted_samples = std::span<const float>{
            decode_storage_.data(),
            static_cast<std::size_t>(result.frame_count) * config_.format.channel_count};
        if (timing_engine_) {
            if (timing_engine_->ConsumeHardResyncRequest()) {
                sink_.HardResync();
                timing_engine_->Reset(rtp_timestamp);
                drift_resampler_.Reset();
                timing_hard_resync_requests_.fetch_add(1U, std::memory_order_relaxed);
            }
            // Closed-loop depth servo. The timing engine already corrects
            // the schedule for the disciplined clock's drift, but two
            // independent crystals remain: the device's render clock and
            // the sender's audio clock. A slow proportional controller on
            // the sink queue depth keeps the feed rate matched to the
            // drain; it never performs hard inserts/drops beyond the tiny
            // bounded ratio.
            double resample_ratio = 1.0;
            const auto timing_diagnostics = timing_engine_->Diagnostics();
            if (timing_diagnostics.locked &&
                timing_diagnostics.target_buffer_microseconds != 0U) {
                const auto target_frames =
                    static_cast<double>(timing_diagnostics.target_buffer_microseconds) *
                    static_cast<double>(config_.format.sample_rate) / 1'000'000.0;
                const auto depth_error =
                    target_frames - static_cast<double>(feedback.queued_frames);
                const auto desired =
                    1.0 + std::clamp(0.05 * depth_error / target_frames, -0.002, 0.002);
                depth_servo_ratio_ += 0.1 * (desired - depth_servo_ratio_);
                resample_ratio = depth_servo_ratio_;
            }
            const auto resampled = drift_resampler_.Process(
                submitted_samples, result.frame_count, config_.format.channel_count,
                resample_ratio, resample_storage_);
            if (!resampled.succeeded) {
                decoder_errors_.fetch_add(1U, std::memory_order_relaxed);
                last_error_.store(8U, std::memory_order_relaxed);
                continue;
            }
            submitted_frames = resampled.output_frames;
            submitted_samples = std::span<const float>{
                resample_storage_.data(),
                static_cast<std::size_t>(submitted_frames) * config_.format.channel_count};
            resampled_input_frames_.fetch_add(result.frame_count,
                                              std::memory_order_relaxed);
            resampled_output_frames_.fetch_add(submitted_frames,
                                               std::memory_order_relaxed);
            if (submitted_frames > result.frame_count) {
                drift_inserted_frames_.fetch_add(submitted_frames - result.frame_count,
                                                 std::memory_order_relaxed);
            } else {
                drift_dropped_frames_.fetch_add(result.frame_count - submitted_frames,
                                                std::memory_order_relaxed);
            }
        }
        const auto target_qpc = timing_engine_
                                    ? timing_engine_->RemoteToLocalQpc(rtp_timestamp)
                                    : std::optional<std::int64_t>{};
        if (timing_engine_ && timing_engine_->RequiresMappedTarget() &&
            !target_qpc.has_value()) {
            timing_unmapped_dropped_frames_.fetch_add(submitted_frames,
                                                      std::memory_order_relaxed);
            continue;
        }
        const audio::DecodedAudioFrameView decoded{
            .interleaved_samples = submitted_samples,
            .frame_count = submitted_frames,
            .rtp_timestamp = rtp_timestamp,
            .extended_sequence_number = packet.extended_sequence_number,
            .target_qpc = target_qpc,
            .concealed = kind == JitterPopKind::Missing,
        };
        if (!SubmitWithBackpressure(decoded)) {
            last_error_.store(7U, std::memory_order_relaxed);
        }
        have_decoded_frame_ = true;
        last_rtp_timestamp_ = rtp_timestamp;
        last_frame_count_ = result.frame_count;
    }
    DrainResendRequests();
}

bool RtpAudioStream::SubmitWithBackpressure(
    const audio::DecodedAudioFrameView& frame) noexcept {
    if (sink_.Submit(frame)) {
        return true;
    }
    sink_backpressure_events_.fetch_add(1U, std::memory_order_relaxed);
    for (std::uint32_t retry = 0U; retry < 100U; ++retry) {
        if (!running_.load(std::memory_order_acquire) ||
            !recording_.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        if (sink_.Submit(frame)) {
            return true;
        }
    }
    // The queue stayed stuck above its high-water target for 100 ms. Dropping
    // this frame alone would splice the next one directly onto the previous
    // waveform (an audible click that bypasses the transition guard). A hard
    // resync fades out, purges the backlog, and fades back in on fresh PCM
    // instead. The timing engine's mapping is untouched, so the stream keeps
    // its presentation timeline.
    sink_.HardResync();
    backpressure_resync_events_.fetch_add(1U, std::memory_order_relaxed);
    return false;
}

void RtpAudioStream::RecordDecodeCost(const std::uint64_t elapsed_microseconds,
                                      const bool margin_sufficient) noexcept {
    const auto bounded_elapsed = std::min<std::uint64_t>(elapsed_microseconds, 60'000'000U);
    const auto current_average =
        decode_processing_average_microseconds_.load(std::memory_order_relaxed);
    const auto next_average = current_average == 0U
                                  ? bounded_elapsed
                                  : (current_average * 15U + bounded_elapsed + 8U) / 16U;
    decode_processing_average_microseconds_.store(next_average,
                                                  std::memory_order_relaxed);

    auto current_maximum =
        decode_processing_maximum_microseconds_.load(std::memory_order_relaxed);
    while (bounded_elapsed > current_maximum &&
           !decode_processing_maximum_microseconds_.compare_exchange_weak(
               current_maximum, bounded_elapsed, std::memory_order_relaxed)) {
    }
    if (!margin_sufficient) {
        decode_budget_miss_count_.fetch_add(1U, std::memory_order_relaxed);
    }
}

void RtpAudioStream::RecordPacketCost(const std::uint64_t elapsed_microseconds) noexcept {
    const auto bounded_elapsed = std::min<std::uint64_t>(elapsed_microseconds, 60'000'000U);
    const auto current_average =
        packet_processing_average_microseconds_.load(std::memory_order_relaxed);
    const auto next_average = current_average == 0U
                                  ? bounded_elapsed
                                  : (current_average * 15U + bounded_elapsed + 8U) / 16U;
    packet_processing_average_microseconds_.store(next_average,
                                                  std::memory_order_relaxed);

    auto current_maximum =
        packet_processing_maximum_microseconds_.load(std::memory_order_relaxed);
    while (bounded_elapsed > current_maximum &&
           !packet_processing_maximum_microseconds_.compare_exchange_weak(
               current_maximum, bounded_elapsed, std::memory_order_relaxed)) {
    }
}

}  // namespace airplaywin::transport
