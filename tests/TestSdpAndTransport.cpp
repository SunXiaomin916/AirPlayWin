#include "TestFramework.h"

#include <string>

#include "core/protocol/RtspTransport.h"
#include "core/protocol/RtpInfo.h"
#include "core/protocol/SdpAudioParser.h"

void TestSdpAndTransport() {
    using airplaywin::protocol::BuildRecordTransportResponse;
    using airplaywin::protocol::ParseRecordTransport;
    using airplaywin::protocol::ParseRtpInfo;
    using airplaywin::protocol::ParseSdpAudioDescription;
    using airplaywin::protocol::ParseSdpAudioSession;
    using airplaywin::protocol::SdpParseError;
    using airplaywin::protocol::TransportParseError;

    const auto dynamic = ParseSdpAudioDescription(
        "v=0\r\n"
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 L16/44100/2\r\n"
        "a=framesize:96 352\r\n");
    APW_EXPECT(dynamic.has_value());
    APW_EXPECT(dynamic->sample_rate == 44'100U);
    APW_EXPECT(dynamic->channel_count == 2U);
    APW_EXPECT(dynamic->payload_type == 96U);
    APW_EXPECT(dynamic->nominal_frames_per_packet == 352U);

    const auto static_l16 = ParseSdpAudioDescription("m=audio 0 RTP/AVP 10\r\n");
    APW_EXPECT(static_l16.has_value());
    APW_EXPECT(static_l16->sample_rate == 44'100U);
    APW_EXPECT(static_l16->channel_count == 2U);

    const auto alac = ParseSdpAudioSession(
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 AppleLossless\r\n"
        "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n"
        "a=rsaaeskey:encrypted-test-key\r\n"
        "a=aesiv:test-initialization-vector\r\n");
    APW_EXPECT(alac.has_value());
    APW_EXPECT(alac->format.codec == airplaywin::audio::AudioCodec::AppleLossless);
    APW_EXPECT(alac->format.sample_rate == 44'100U);
    APW_EXPECT(alac->format.channel_count == 2U);
    APW_EXPECT(alac->format.nominal_frames_per_packet == 352U);
    APW_EXPECT(alac->format.codec_config_size == 24U);
    APW_EXPECT(alac->format.encrypted);
    APW_EXPECT(alac->encrypted_aes_key == "encrypted-test-key");
    APW_EXPECT(alac->aes_initialization_vector == "test-initialization-vector");
    APW_EXPECT(std::to_integer<std::uint8_t>(alac->format.codec_config[0U]) == 0U);
    APW_EXPECT(std::to_integer<std::uint8_t>(alac->format.codec_config[2U]) == 1U);
    APW_EXPECT(std::to_integer<std::uint8_t>(alac->format.codec_config[3U]) == 96U);
    APW_EXPECT(std::to_integer<std::uint8_t>(alac->format.codec_config[5U]) == 16U);
    APW_EXPECT(std::to_integer<std::uint8_t>(alac->format.codec_config[9U]) == 2U);

    const auto missing_fmtp = ParseSdpAudioDescription(
        "m=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\n");
    APW_EXPECT(!missing_fmtp.has_value());
    APW_EXPECT(missing_fmtp.error() == SdpParseError::InvalidCodecConfiguration);

    const auto incomplete_crypto = ParseSdpAudioDescription(
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 AppleLossless\r\n"
        "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n"
        "a=rsaaeskey:encrypted-test-key\r\n");
    APW_EXPECT(!incomplete_crypto.has_value());
    APW_EXPECT(incomplete_crypto.error() == SdpParseError::InvalidCodecConfiguration);

    const auto transport = ParseRecordTransport(
        "RTP/AVP/UDP;unicast;mode=record;control_port=6001;timing_port=6002");
    APW_EXPECT(transport.has_value());
    APW_EXPECT(transport->client_control_port == 6'001U);
    APW_EXPECT(transport->client_timing_port == 6'002U);
    APW_EXPECT(BuildRecordTransportResponse(7'000U, 7'001U, 7'002U) ==
               "RTP/AVP/UDP;unicast;mode=record;server_port=7000;control_port=7001;"
               "timing_port=7002");

    const auto multicast = ParseRecordTransport("RTP/AVP/UDP;multicast;mode=record");
    APW_EXPECT(!multicast.has_value());
    APW_EXPECT(multicast.error() == TransportParseError::MulticastUnsupported);
    const auto bad_port =
        ParseRecordTransport("RTP/AVP/UDP;unicast;control_port=70000");
    APW_EXPECT(!bad_port.has_value());
    APW_EXPECT(bad_port.error() == TransportParseError::InvalidPort);
    const auto bad_mode = ParseRecordTransport("RTP/AVP/UDP;unicast;mode=play");
    APW_EXPECT(!bad_mode.has_value());
    APW_EXPECT(bad_mode.error() == TransportParseError::InvalidMode);

    const auto rtp_info = ParseRtpInfo(
        "url=rtsp://127.0.0.1/stream;seq=65535;rtptime=4294967295");
    APW_EXPECT(rtp_info.has_value());
    APW_EXPECT(rtp_info->sequence_number == 65'535U);
    APW_EXPECT(rtp_info->rtp_timestamp == 4'294'967'295U);
    APW_EXPECT(!ParseRtpInfo("seq=-1;rtptime=5").has_value());
    APW_EXPECT(!ParseRtpInfo("seq=65536;rtptime=5").has_value());
    APW_EXPECT(!ParseRtpInfo("seq=1;rtptime=4294967296").has_value());
    APW_EXPECT(!ParseRtpInfo("seq=1;seq=2").has_value());
    APW_EXPECT(!ParseRtpInfo("url=rtsp://one,seq=2").has_value());
}
