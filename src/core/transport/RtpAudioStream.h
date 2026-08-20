#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/audio/IAudioDecoder.h"
#include "core/audio/IAudioFrameSink.h"
#include "core/transport/IAudioStream.h"
#include "core/transport/RtpJitterBuffer.h"

namespace airplaywin::transport {

struct RtpAudioStreamConfig final {
    ConnectionId connection_id{0U};
    audio::EncodedAudioFormat format{};
    RtpJitterBufferConfig jitter_buffer{};
    std::uint32_t allowed_peer_ipv4_network_order{0U};
};

class RtpAudioStream final : public IAudioStream {
public:
    RtpAudioStream(RtpAudioStreamConfig config,
                   std::unique_ptr<audio::IAudioDecoder> decoder,
                   audio::IAudioFrameSink& sink);
    ~RtpAudioStream() override;

    RtpAudioStream(const RtpAudioStream&) = delete;
    RtpAudioStream& operator=(const RtpAudioStream&) = delete;

    [[nodiscard]] bool Start() override;
    [[nodiscard]] bool Record(const AudioTimelineAnchor& anchor) override;
    void Pause() noexcept override;
    void Resume(const AudioTimelineAnchor& anchor) noexcept override;
    void Flush(const AudioTimelineAnchor& anchor) noexcept override;
    void SetVolume(float linear_gain) noexcept override;
    void Stop() noexcept override;
    void OnDatagram(std::span<const std::byte> datagram,
                    const DatagramEndpoint& source,
                    std::int64_t arrival_time_nanoseconds) noexcept override;
    [[nodiscard]] AudioTransportDiagnostics Diagnostics() const noexcept override;

private:
    void WorkerLoop() noexcept;
    void ProcessAvailablePackets() noexcept;
    void ResetPacketTimeline(const AudioTimelineAnchor& anchor) noexcept;
    [[nodiscard]] bool IsAtOrAfterTimeline(const RtpPacketView& packet) const noexcept;
    [[nodiscard]] bool SubmitWithBackpressure(const audio::DecodedAudioFrameView& frame) noexcept;

    RtpAudioStreamConfig config_{};
    std::unique_ptr<audio::IAudioDecoder> decoder_;
    audio::IAudioFrameSink& sink_;
    RtpJitterBuffer jitter_buffer_;
    std::vector<float> decode_storage_{};
    mutable std::mutex state_mutex_{};
    std::mutex processing_mutex_{};
    mutable std::mutex timeline_mutex_{};
    std::condition_variable wake_{};
    std::thread worker_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> configured_{false};
    std::atomic<std::uint64_t> stream_source_{0U};
    std::uint32_t last_rtp_timestamp_{0U};
    std::uint32_t last_frame_count_{0U};
    bool have_decoded_frame_{false};
    std::atomic<std::uint64_t> datagrams_received_{0U};
    std::atomic<std::uint64_t> datagram_bytes_{0U};
    std::atomic<std::uint64_t> invalid_rtp_packets_{0U};
    std::atomic<std::uint64_t> unexpected_source_packets_{0U};
    std::atomic<std::uint64_t> unexpected_payload_packets_{0U};
    std::atomic<std::uint64_t> retransmitted_packets_{0U};
    std::atomic<std::uint64_t> timeline_rejected_packets_{0U};
    std::atomic<std::uint64_t> timeline_resets_{0U};
    std::atomic<std::uint32_t> sequence_anchor_{0U};
    std::atomic<std::uint64_t> timestamp_anchor_{0U};
    std::atomic<std::uint64_t> decoded_packets_{0U};
    std::atomic<std::uint64_t> decoded_frames_{0U};
    std::atomic<std::uint64_t> concealed_packets_{0U};
    std::atomic<std::uint64_t> concealed_frames_{0U};
    std::atomic<std::uint64_t> decoder_errors_{0U};
    std::atomic<std::uint64_t> sink_backpressure_events_{0U};
    std::atomic<std::uint32_t> last_error_{0U};
};

}  // namespace airplaywin::transport
