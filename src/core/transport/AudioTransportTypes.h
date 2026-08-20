#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/audio/AudioStreamTypes.h"
#include "core/timing/TimingTypes.h"
#include "core/transport/IControlConnectionHandler.h"
#include "core/transport/RtpJitterBuffer.h"

namespace airplaywin::transport {

// A sender-provided lower bound for a new RTP timeline. Sequence numbers and
// timestamps use their native wrapping domains and are compared wrap-aware.
struct AudioTimelineAnchor final {
    std::optional<std::uint16_t> sequence_number{};
    std::optional<std::uint32_t> rtp_timestamp{};

    [[nodiscard]] constexpr bool IsEmpty() const noexcept {
        return !sequence_number.has_value() && !rtp_timestamp.has_value();
    }
};

struct AudioTransportSetupRequest final {
    ConnectionId connection_id{0U};
    std::string peer_address{};
    audio::EncodedAudioFormat format{};
    std::uint16_t client_control_port{0U};
    std::uint16_t client_timing_port{0U};
};

enum class AudioTransportSetupError : std::uint8_t {
    None,
    InvalidRequest,
    UnsupportedFormat,
    AlreadyConfigured,
    SocketFailure,
    AudioSinkFailure,
};

struct AudioTransportSetupResult final {
    AudioTransportSetupError error{AudioTransportSetupError::InvalidRequest};
    std::uint16_t server_audio_port{0U};
    std::uint16_t server_control_port{0U};
    std::uint16_t server_timing_port{0U};

    [[nodiscard]] constexpr bool Succeeded() const noexcept {
        return error == AudioTransportSetupError::None && server_audio_port != 0U;
    }
};

struct AudioTransportDiagnostics final {
    bool configured{false};
    bool recording{false};
    ConnectionId connection_id{0U};
    std::uint16_t server_audio_port{0U};
    std::uint16_t server_control_port{0U};
    std::uint16_t server_timing_port{0U};
    std::uint64_t datagrams_received{0U};
    std::uint64_t datagram_bytes{0U};
    std::uint64_t invalid_rtp_packets{0U};
    std::uint64_t unexpected_source_packets{0U};
    std::uint64_t unexpected_payload_packets{0U};
    std::uint64_t retransmitted_packets{0U};
    std::uint64_t timeline_rejected_packets{0U};
    std::uint64_t timeline_resets{0U};
    bool has_sequence_anchor{false};
    bool has_timestamp_anchor{false};
    std::uint16_t sequence_anchor{0U};
    std::uint32_t timestamp_anchor{0U};
    std::uint64_t decoded_packets{0U};
    std::uint64_t decoded_frames{0U};
    std::uint64_t concealed_packets{0U};
    std::uint64_t concealed_frames{0U};
    std::uint64_t decoder_errors{0U};
    std::uint64_t sink_backpressure_events{0U};
    std::uint64_t control_datagrams{0U};
    std::uint64_t timing_datagrams{0U};
    RtpJitterBufferDiagnostics jitter_buffer{};
    timing::TimingDiagnostics timing{};
    std::uint32_t last_error{0U};
};

}  // namespace airplaywin::transport
