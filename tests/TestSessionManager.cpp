#include "TestFramework.h"

#include "core/session/SessionManager.h"

void TestSessionManager() {
    using airplaywin::session::ActivationResult;
    using airplaywin::session::ActiveSessionPolicy;
    using airplaywin::session::SessionManager;
    using airplaywin::session::SessionState;

    SessionManager rejecting{ActiveSessionPolicy::RejectNew};
    APW_EXPECT(rejecting.Create(1U, "127.0.0.1"));
    APW_EXPECT(rejecting.Create(2U, "192.0.2.2"));
    APW_EXPECT(!rejecting.Create(2U, "duplicate"));
    APW_EXPECT(rejecting.RecordRequest(1U, "ANNOUNCE", "sender-a"));
    APW_EXPECT(rejecting.Transition(1U, SessionState::Connected, SessionState::Announced));
    APW_EXPECT(rejecting.Activate(1U) == ActivationResult::Activated);
    APW_EXPECT(rejecting.Activate(1U) == ActivationResult::AlreadyActive);
    APW_EXPECT(rejecting.Activate(2U) == ActivationResult::Rejected);
    APW_EXPECT(rejecting.SetVolume(1U, -12.5));
    const auto first = rejecting.Get(1U);
    APW_EXPECT(first.has_value());
    APW_EXPECT(first->owns_playback);
    APW_EXPECT(first->sender_identity == "sender-a");
    APW_EXPECT_NEAR(first->volume_db, -12.5, 0.0001);
    rejecting.Close(1U);
    APW_EXPECT(rejecting.Activate(2U) == ActivationResult::Activated);
    const auto rejecting_diagnostics = rejecting.Diagnostics();
    APW_EXPECT(rejecting_diagnostics.tracked_sessions == 1U);
    APW_EXPECT(rejecting_diagnostics.rejected_sessions == 1U);

    SessionManager preempting{ActiveSessionPolicy::PreemptExisting};
    APW_EXPECT(preempting.Create(10U, "a"));
    APW_EXPECT(preempting.Create(11U, "b"));
    APW_EXPECT(preempting.Activate(10U) == ActivationResult::Activated);
    APW_EXPECT(preempting.Activate(11U) == ActivationResult::PreemptedExisting);
    const auto preempted = preempting.Get(10U);
    APW_EXPECT(preempted.has_value());
    APW_EXPECT(preempted->state == SessionState::Closing);
    APW_EXPECT(!preempted->owns_playback);
    APW_EXPECT(preempting.Diagnostics().preempted_sessions == 1U);
}
