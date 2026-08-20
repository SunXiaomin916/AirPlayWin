#pragma once

#include <optional>
#include <string_view>

#include "core/transport/AudioTransportTypes.h"

namespace airplaywin::protocol {

// Parses the single-stream RTP-Info form used by the audio control session.
// Multi-stream values and duplicate/unknown-only anchors are rejected.
[[nodiscard]] std::optional<transport::AudioTimelineAnchor> ParseRtpInfo(
    std::string_view value) noexcept;

}  // namespace airplaywin::protocol
