#pragma once

#include "core/transport/AudioTransportTypes.h"

namespace airplaywin::transport {

class IAudioTransportController {
public:
    virtual ~IAudioTransportController() = default;

    [[nodiscard]] virtual AudioTransportSetupResult Setup(
        const AudioTransportSetupRequest& request) = 0;
    [[nodiscard]] virtual bool Record(ConnectionId connection_id) = 0;
    [[nodiscard]] virtual bool Pause(ConnectionId connection_id) noexcept = 0;
    [[nodiscard]] virtual bool Resume(ConnectionId connection_id) noexcept = 0;
    [[nodiscard]] virtual bool Flush(ConnectionId connection_id) noexcept = 0;
    virtual void SetVolume(ConnectionId connection_id, float linear_gain) noexcept = 0;
    virtual void Teardown(ConnectionId connection_id) noexcept = 0;
    [[nodiscard]] virtual AudioTransportDiagnostics Diagnostics() const noexcept = 0;
};

}  // namespace airplaywin::transport
