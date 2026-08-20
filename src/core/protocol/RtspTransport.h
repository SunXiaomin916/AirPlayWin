#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace airplaywin::protocol {

enum class TransportParseError : std::uint8_t {
    Empty,
    UnsupportedProtocol,
    MulticastUnsupported,
    InvalidMode,
    InvalidPort,
    DuplicateParameter,
    MalformedParameter,
};

struct RtspTransportParameters final {
    std::uint16_t client_control_port{0U};
    std::uint16_t client_timing_port{0U};
};

[[nodiscard]] std::expected<RtspTransportParameters, TransportParseError>
ParseRecordTransport(std::string_view header) noexcept;

[[nodiscard]] std::string BuildRecordTransportResponse(std::uint16_t server_audio_port,
                                                       std::uint16_t server_control_port,
                                                       std::uint16_t server_timing_port);

}  // namespace airplaywin::protocol
