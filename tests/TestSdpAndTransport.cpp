#include "TestFramework.h"

#include <string>

#include "core/protocol/RtspTransport.h"
#include "core/protocol/SdpAudioParser.h"

void TestSdpAndTransport() {
    using airplaywin::protocol::BuildRecordTransportResponse;
    using airplaywin::protocol::ParseRecordTransport;
    using airplaywin::protocol::ParseSdpAudioDescription;
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

    const auto unsupported = ParseSdpAudioDescription(
        "m=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless/44100/2\r\n");
    APW_EXPECT(!unsupported.has_value());
    APW_EXPECT(unsupported.error() == SdpParseError::UnsupportedCodec);

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
}
