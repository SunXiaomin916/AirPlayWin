#include "platform/windows/network/WindowsRtpTransportController.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <memory>
#include <utility>

#include "core/audio/PcmL16Decoder.h"
#include "core/transport/RtpAudioStream.h"

namespace airplaywin::windows::network {

WindowsRtpTransportController::WindowsRtpTransportController(
    airplaywin::audio::IAudioFrameSink& sink,
    WindowsRtpTransportOptions options)
    : sink_(sink), options_(std::move(options)) {}

WindowsRtpTransportController::~WindowsRtpTransportController() {
    std::scoped_lock lock{mutex_};
    TeardownLocked();
}

airplaywin::transport::AudioTransportSetupResult WindowsRtpTransportController::Setup(
    const airplaywin::transport::AudioTransportSetupRequest& request) {
    using airplaywin::transport::AudioTransportSetupError;
    using airplaywin::transport::AudioTransportSetupResult;
    if (request.connection_id == 0U || !request.format.IsValid() ||
        request.format.codec != airplaywin::audio::AudioCodec::PcmL16BigEndian ||
        options_.jitter_capacity_packets < 4U || options_.jitter_target_packets == 0U ||
        options_.jitter_target_packets >= options_.jitter_capacity_packets) {
        return {.error = AudioTransportSetupError::InvalidRequest};
    }
    std::scoped_lock lock{mutex_};
    if (connection_id_ != 0U) {
        return {.error = connection_id_ == request.connection_id
                             ? AudioTransportSetupError::AlreadyConfigured
                             : AudioTransportSetupError::InvalidRequest};
    }
    in_addr allowed_peer{};
    if (InetPtonA(AF_INET, request.peer_address.c_str(), &allowed_peer) != 1) {
        return {.error = AudioTransportSetupError::InvalidRequest};
    }

    auto stream = std::make_unique<airplaywin::transport::RtpAudioStream>(
        airplaywin::transport::RtpAudioStreamConfig{
            .connection_id = request.connection_id,
            .format = request.format,
            .jitter_buffer =
                {.capacity_packets = options_.jitter_capacity_packets,
                 .target_packets = options_.jitter_target_packets,
                 .clock_rate = request.format.sample_rate},
            .allowed_peer_ipv4_network_order = allowed_peer.s_addr,
        },
        std::make_unique<airplaywin::audio::PcmL16Decoder>(), sink_);
    if (!stream->Start()) {
        last_error_ = stream->Diagnostics().last_error;
        return {.error = AudioTransportSetupError::AudioSinkFailure};
    }

    auto audio_receiver = std::make_unique<IocpUdpReceiver>(*stream);
    control_handler_.Reset();
    control_handler_.SetStream(stream.get());
    timing_handler_.Reset();
    auto control_receiver = std::make_unique<IocpUdpReceiver>(control_handler_);
    auto timing_receiver = std::make_unique<IocpUdpReceiver>(timing_handler_);
    const IocpUdpReceiverOptions receiver_options{
        .bind_address = options_.bind_address,
        .port = 0U,
        .receive_depth = 4U,
        .max_datagram_bytes = 8U * 1'024U,
    };
    if (!audio_receiver->Start(receiver_options) ||
        !control_receiver->Start(receiver_options) ||
        !timing_receiver->Start(receiver_options)) {
        const auto audio_error = audio_receiver->Diagnostics().last_error;
        const auto control_error = control_receiver->Diagnostics().last_error;
        const auto timing_error = timing_receiver->Diagnostics().last_error;
        last_error_ = audio_error != 0U ? audio_error
                                       : (control_error != 0U ? control_error : timing_error);
        timing_receiver->Stop();
        control_receiver->Stop();
        audio_receiver->Stop();
        control_handler_.SetStream(nullptr);
        stream->Stop();
        return {.error = AudioTransportSetupError::SocketFailure};
    }

    connection_id_ = request.connection_id;
    stream_ = std::move(stream);
    audio_receiver_ = std::move(audio_receiver);
    control_receiver_ = std::move(control_receiver);
    timing_receiver_ = std::move(timing_receiver);
    last_error_ = 0U;
    return AudioTransportSetupResult{
        .error = AudioTransportSetupError::None,
        .server_audio_port = audio_receiver_->BoundPort(),
        .server_control_port = control_receiver_->BoundPort(),
        .server_timing_port = timing_receiver_->BoundPort(),
    };
}

bool WindowsRtpTransportController::Record(
    const airplaywin::transport::ConnectionId connection_id,
    const airplaywin::transport::AudioTimelineAnchor& anchor) {
    std::scoped_lock lock{mutex_};
    return stream_ && connection_id_ == connection_id && stream_->Record(anchor);
}

bool WindowsRtpTransportController::Pause(
    const airplaywin::transport::ConnectionId connection_id) noexcept {
    std::scoped_lock lock{mutex_};
    if (!stream_ || connection_id_ != connection_id) {
        return false;
    }
    stream_->Pause();
    return true;
}

bool WindowsRtpTransportController::Resume(
    const airplaywin::transport::ConnectionId connection_id,
    const airplaywin::transport::AudioTimelineAnchor& anchor) noexcept {
    std::scoped_lock lock{mutex_};
    if (!stream_ || connection_id_ != connection_id) {
        return false;
    }
    stream_->Resume(anchor);
    return true;
}

bool WindowsRtpTransportController::Flush(
    const airplaywin::transport::ConnectionId connection_id,
    const airplaywin::transport::AudioTimelineAnchor& anchor) noexcept {
    std::scoped_lock lock{mutex_};
    if (!stream_ || connection_id_ != connection_id) {
        return false;
    }
    stream_->Flush(anchor);
    return true;
}

void WindowsRtpTransportController::SetVolume(
    const airplaywin::transport::ConnectionId connection_id,
    const float linear_gain) noexcept {
    std::scoped_lock lock{mutex_};
    if (stream_ && connection_id_ == connection_id) {
        stream_->SetVolume(linear_gain);
    }
}

void WindowsRtpTransportController::Teardown(
    const airplaywin::transport::ConnectionId connection_id) noexcept {
    std::scoped_lock lock{mutex_};
    if (connection_id_ == connection_id) {
        TeardownLocked();
    }
}

airplaywin::transport::AudioTransportDiagnostics
WindowsRtpTransportController::Diagnostics() const noexcept {
    std::scoped_lock lock{mutex_};
    auto diagnostics = stream_ ? stream_->Diagnostics()
                               : airplaywin::transport::AudioTransportDiagnostics{};
    diagnostics.configured = stream_ != nullptr;
    diagnostics.connection_id = connection_id_;
    diagnostics.server_audio_port = audio_receiver_ ? audio_receiver_->BoundPort() : 0U;
    diagnostics.server_control_port = control_receiver_ ? control_receiver_->BoundPort() : 0U;
    diagnostics.server_timing_port = timing_receiver_ ? timing_receiver_->BoundPort() : 0U;
    diagnostics.control_datagrams = control_handler_.Count();
    diagnostics.timing_datagrams = timing_handler_.Count();
    diagnostics.last_error = last_error_ == 0U ? diagnostics.last_error : last_error_;
    return diagnostics;
}

void WindowsRtpTransportController::CountingHandler::OnDatagram(
    const std::span<const std::byte> datagram,
    const airplaywin::transport::DatagramEndpoint& source,
    const std::int64_t arrival_time_nanoseconds) noexcept {
    static_cast<void>(datagram);
    static_cast<void>(source);
    static_cast<void>(arrival_time_nanoseconds);
    count_.fetch_add(1U, std::memory_order_relaxed);
}

std::uint64_t WindowsRtpTransportController::CountingHandler::Count() const noexcept {
    return count_.load(std::memory_order_relaxed);
}

void WindowsRtpTransportController::CountingHandler::Reset() noexcept {
    count_.store(0U, std::memory_order_relaxed);
}

void WindowsRtpTransportController::RetransmitHandler::OnDatagram(
    const std::span<const std::byte> datagram,
    const airplaywin::transport::DatagramEndpoint& source,
    const std::int64_t arrival_time_nanoseconds) noexcept {
    CountingHandler::OnDatagram(datagram, source, arrival_time_nanoseconds);
    constexpr std::uint8_t kRetransmittedPayloadType = 0x56U;
    if (datagram.size() >= 4U &&
        (std::to_integer<std::uint8_t>(datagram[1U]) & 0x7FU) ==
            kRetransmittedPayloadType) {
        if (auto* const stream = stream_.load(std::memory_order_acquire); stream != nullptr) {
            stream->OnDatagram(datagram, source, arrival_time_nanoseconds);
        }
    }
}

void WindowsRtpTransportController::RetransmitHandler::SetStream(
    airplaywin::transport::IAudioStream* const stream) noexcept {
    stream_.store(stream, std::memory_order_release);
}

void WindowsRtpTransportController::TeardownLocked() noexcept {
    if (timing_receiver_) {
        timing_receiver_->Stop();
    }
    if (control_receiver_) {
        control_receiver_->Stop();
    }
    control_handler_.SetStream(nullptr);
    if (audio_receiver_) {
        audio_receiver_->Stop();
    }
    if (stream_) {
        stream_->Stop();
    }
    timing_receiver_.reset();
    control_receiver_.reset();
    audio_receiver_.reset();
    stream_.reset();
    connection_id_ = 0U;
}

}  // namespace airplaywin::windows::network
