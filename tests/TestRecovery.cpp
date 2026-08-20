#include "TestFramework.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/crypto/OpenSessionAuthenticator.h"
#include "core/lifecycle/RecoveryCoordinator.h"
#include "core/protocol/AirPlayControlService.h"

namespace {

[[nodiscard]] std::vector<std::byte> Bytes(const std::string_view text) {
    const auto* const begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

}  // namespace

void TestRecovery() {
    using airplaywin::lifecycle::ReceiverRuntimeState;
    using airplaywin::lifecycle::RecoveryCoordinator;

    RecoveryCoordinator recovery;
    APW_EXPECT(recovery.Diagnostics().state == ReceiverRuntimeState::Running);
    recovery.OnSuspend();
    recovery.OnSuspend();
    APW_EXPECT(recovery.IsSuspended());
    APW_EXPECT(recovery.Diagnostics().recovery_generation == 1U);
    recovery.OnResume();
    APW_EXPECT(recovery.RecoveryRequired());
    APW_EXPECT(recovery.Diagnostics().recovery_generation == 2U);
    recovery.RecordRecoveryResult(false, 55U);
    APW_EXPECT(recovery.RecoveryRequired());
    APW_EXPECT(recovery.Diagnostics().last_error == 55U);
    recovery.RecordRecoveryResult(true);
    APW_EXPECT(recovery.Diagnostics().state == ReceiverRuntimeState::Running);
    recovery.RequestRecovery(77U);
    APW_EXPECT(recovery.RecoveryRequired());
    recovery.RecordRecoveryResult(true);
    const auto recovered = recovery.Diagnostics();
    APW_EXPECT(recovered.suspend_events == 2U);
    APW_EXPECT(recovered.resume_events == 1U);
    APW_EXPECT(recovered.recovery_attempts == 3U);
    APW_EXPECT(recovered.recovery_successes == 2U);
    APW_EXPECT(recovered.recovery_failures == 1U);
    recovery.Stop();
    recovery.OnResume();
    APW_EXPECT(recovery.Diagnostics().state == ReceiverRuntimeState::Stopped);

    airplaywin::crypto::OpenSessionAuthenticator authenticator;
    airplaywin::protocol::AirPlayControlService service{authenticator};
    constexpr std::string_view sdp =
        "v=0\r\nm=audio 0 RTP/AVP 96\r\na=rtpmap:96 L16/44100/2\r\n"
        "a=framesize:96 352\r\n";
    const std::string announce =
        "ANNOUNCE rtsp://127.0.0.1/stream RTSP/1.0\r\n"
        "CSeq: 1\r\nContent-Type: application/sdp\r\nContent-Length: " +
        std::to_string(sdp.size()) + "\r\n\r\n" + std::string{sdp};
    for (std::uint64_t iteration = 1U; iteration <= 100U; ++iteration) {
        service.OnConnected(iteration, "127.0.0.1");
        const auto reply = service.OnBytes(iteration, Bytes(announce));
        APW_EXPECT(reply.writes.size() == 1U);
        service.OnDisconnected(iteration,
                               airplaywin::transport::DisconnectReason::PeerClosed);
    }
    service.OnConnected(101U, "127.0.0.1");
    APW_EXPECT(service.OnBytes(101U, Bytes(announce)).writes.size() == 1U);
    service.OnDisconnected(101U,
                           airplaywin::transport::DisconnectReason::ServerShutdown);
    const auto control = service.Diagnostics();
    APW_EXPECT(control.sessions.active_connections == 0U);
    APW_EXPECT(control.sessions.created_sessions == 101U);
    APW_EXPECT(control.sessions.closed_sessions == 101U);
    APW_EXPECT(control.peer_disconnects == 100U);
    APW_EXPECT(control.shutdown_disconnects == 1U);
}
