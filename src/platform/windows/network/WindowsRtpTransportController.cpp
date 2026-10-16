#include "platform/windows/network/WindowsRtpTransportController.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

#include "core/audio/PcmL16Decoder.h"
#include "core/timing/BufferedRtpTimingEngine.h"
#include "core/timing/DisciplinedRtpTimingEngine.h"
#include "core/transport/RtpAudioStream.h"
#include "platform/windows/audio/WindowsAlacDecoder.h"
#include "platform/windows/timing/QpcClock.h"

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
    const airplaywin::transport::RtpJitterBufferConfig jitter_config{
        .capacity_packets = options_.jitter_capacity_packets,
        .target_packets = options_.jitter_target_packets,
        .clock_rate = request.format.sample_rate,
        .adaptive_enabled = options_.enable_adaptive_jitter,
        .minimum_target_packets = options_.jitter_minimum_packets,
        .maximum_target_packets = options_.jitter_maximum_packets,
        .stable_window_packets = options_.jitter_stable_window_packets,
        .recovery_window_packets = options_.jitter_recovery_window_packets,
    };
    if (request.connection_id == 0U || !request.format.IsValid() ||
        !jitter_config.IsValid() ||
        (options_.enable_buffered_timing && options_.enable_ptp_timing) ||
        (options_.enable_ptp_timing && !options_.ptp_clock_domain) ||
        ((options_.enable_buffered_timing || options_.enable_ptp_timing) &&
         (options_.buffered_timing_milliseconds < 20U ||
          options_.buffered_timing_milliseconds > 2'000U))) {
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

    // Classic RAOP retransmission: the sender retransmits only when asked.
    // Prepare the bounded send path to the client's negotiated control port.
    resend_target_valid_ = false;
    resend_target_ = {};
    SOCKET resend_socket = INVALID_SOCKET;
    if (request.client_control_port != 0U) {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(request.client_control_port);
        target.sin_addr.s_addr = allowed_peer.s_addr;
        resend_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (resend_socket != INVALID_SOCKET) {
            const DWORD send_timeout_ms = 100U;
            static_cast<void>(setsockopt(resend_socket, SOL_SOCKET, SO_SNDTIMEO,
                                         reinterpret_cast<const char*>(&send_timeout_ms),
                                         static_cast<int>(sizeof(send_timeout_ms))));
            resend_target_ = target;
            resend_target_valid_ = true;
        }
    }

    std::unique_ptr<airplaywin::timing::ITimingEngine> timing_engine;
    if (options_.enable_ptp_timing) {
        timing_engine =
            std::make_unique<airplaywin::timing::DisciplinedRtpTimingEngine>(
                airplaywin::timing::DisciplinedRtpTimingConfig{
                    .remote_clock_rate = request.format.sample_rate,
                    .target_buffer_milliseconds =
                        options_.buffered_timing_milliseconds,
                    .endpoint_latency_offset_microseconds =
                        options_.endpoint_latency_offset_microseconds,
                    .require_phase_anchor = options_.group_phase_timeline != nullptr,
                },
                options_.ptp_clock_domain,
                std::make_unique<airplaywin::windows::timing::QpcClockSource>(),
                options_.group_phase_timeline);
    } else if (options_.enable_buffered_timing) {
        timing_engine = std::make_unique<airplaywin::timing::BufferedRtpTimingEngine>(
            airplaywin::timing::BufferedRtpTimingConfig{
                .remote_clock_rate = request.format.sample_rate,
                .target_buffer_milliseconds = options_.buffered_timing_milliseconds,
            },
            std::make_unique<airplaywin::windows::timing::QpcClockSource>());
    }
    std::unique_ptr<airplaywin::audio::IAudioDecoder> decoder;
    if (request.format.codec == airplaywin::audio::AudioCodec::AppleLossless) {
        decoder = std::make_unique<airplaywin::windows::audio::WindowsAlacDecoder>();
    } else if (request.format.codec == airplaywin::audio::AudioCodec::PcmL16BigEndian) {
        decoder = std::make_unique<airplaywin::audio::PcmL16Decoder>();
    } else {
        return {.error = AudioTransportSetupError::InvalidRequest};
    }
    std::function<void(std::uint16_t, std::uint16_t)> resend_requester;
    if (resend_socket != INVALID_SOCKET) {
        resend_requester = [this](const std::uint16_t first_sequence,
                                  const std::uint16_t count) {
            SendResendRequest(first_sequence, count);
        };
    }
    auto stream = std::make_unique<airplaywin::transport::RtpAudioStream>(
        airplaywin::transport::RtpAudioStreamConfig{
            .connection_id = request.connection_id,
            .format = request.format,
            .jitter_buffer = jitter_config,
            .protocol_latency_frames = request.protocol_latency_frames,
            .allowed_peer_ipv4_network_order = allowed_peer.s_addr,
            .resend_requester = std::move(resend_requester),
        },
        std::move(decoder), sink_, std::move(timing_engine));
    if (!stream->Start()) {
        last_error_ = stream->Diagnostics().last_error;
        if (resend_socket != INVALID_SOCKET) {
            closesocket(resend_socket);
        }
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
        // Maximum UDP payload; large enough for any negotiated ALAC packet.
        .max_datagram_bytes = 65'507U,
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
        if (resend_socket != INVALID_SOCKET) {
            closesocket(resend_socket);
        }
        return {.error = AudioTransportSetupError::SocketFailure};
    }

    resend_socket_ = resend_socket;
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
    // The stream worker owns the resend requester; join it before the
    // socket and target are released.
    if (stream_) {
        stream_->Stop();
    }
    if (resend_socket_ != INVALID_SOCKET) {
        closesocket(resend_socket_);
        resend_socket_ = INVALID_SOCKET;
    }
    resend_target_valid_ = false;
    resend_target_ = {};
    timing_receiver_.reset();
    control_receiver_.reset();
    audio_receiver_.reset();
    stream_.reset();
    connection_id_ = 0U;
}

void WindowsRtpTransportController::SendResendRequest(
    const std::uint16_t first_sequence, const std::uint16_t count) noexcept {
    if (resend_socket_ == INVALID_SOCKET || !resend_target_valid_) {
        return;
    }
    // Classic RAOP resend request: 0x80, 0xD5, own sequence 1, then the
    // first missing sequence number and the count of consecutive packets.
    const std::array<std::byte, 8U> request{
        std::byte{0x80},
        std::byte{0xD5},
        std::byte{0x00},
        std::byte{0x01},
        std::byte{static_cast<std::uint8_t>(first_sequence >> 8U)},
        std::byte{static_cast<std::uint8_t>(first_sequence & 0xFFU)},
        std::byte{static_cast<std::uint8_t>(count >> 8U)},
        std::byte{static_cast<std::uint8_t>(count & 0xFFU)},
    };
    const auto sent = sendto(resend_socket_, reinterpret_cast<const char*>(request.data()),
                             static_cast<int>(request.size()), 0,
                             reinterpret_cast<const sockaddr*>(&resend_target_),
                             static_cast<int>(sizeof(resend_target_)));
    if (sent == SOCKET_ERROR) {
        resend_send_errors_.fetch_add(1U, std::memory_order_relaxed);
    }
}

}  // namespace airplaywin::windows::network
