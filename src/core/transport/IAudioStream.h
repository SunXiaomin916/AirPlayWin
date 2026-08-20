#pragma once

#include "core/transport/AudioTransportTypes.h"
#include "core/transport/IUdpDatagramHandler.h"

namespace airplaywin::transport {

class IAudioStream : public IUdpDatagramHandler {
public:
    ~IAudioStream() override = default;

    [[nodiscard]] virtual bool Start() = 0;
    [[nodiscard]] virtual bool Record(const AudioTimelineAnchor& anchor) = 0;
    virtual void Pause() noexcept = 0;
    virtual void Resume(const AudioTimelineAnchor& anchor) noexcept = 0;
    virtual void Flush(const AudioTimelineAnchor& anchor) noexcept = 0;
    virtual void SetVolume(float linear_gain) noexcept = 0;
    virtual void Stop() noexcept = 0;
    [[nodiscard]] virtual AudioTransportDiagnostics Diagnostics() const noexcept = 0;
};

}  // namespace airplaywin::transport
