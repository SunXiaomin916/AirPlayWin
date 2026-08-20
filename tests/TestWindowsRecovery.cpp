#include "TestFramework.h"

#include <Windows.h>

#include <string>

#include "platform/windows/system/SingleInstanceGuard.h"
#include "platform/windows/system/WindowsFirewallManager.h"
#include "platform/windows/system/WindowsPowerEventMonitor.h"

void TestWindowsRecovery() {
    using airplaywin::windows::system::FirewallRuleHealth;
    using airplaywin::windows::system::FirewallRuleSpec;
    using airplaywin::windows::system::PowerLifecycleEvent;
    using airplaywin::windows::system::SingleInstanceGuard;
    using airplaywin::windows::system::WindowsFirewallManager;
    using airplaywin::windows::system::WindowsPowerEventMonitor;

    WindowsPowerEventMonitor monitor;
    APW_EXPECT(monitor.Start());
    monitor.RecordPowerBroadcast(PBT_APMSUSPEND);
    monitor.RecordPowerBroadcast(PBT_APMSUSPEND);
    monitor.RecordPowerBroadcast(PBT_APMRESUMEAUTOMATIC);
    monitor.RecordPowerBroadcast(PBT_APMRESUMECRITICAL);
    const auto suspend = monitor.Poll();
    const auto resume = monitor.Poll();
    APW_EXPECT(suspend == PowerLifecycleEvent::Suspend);
    APW_EXPECT(resume == PowerLifecycleEvent::Resume);
    APW_EXPECT(!monitor.Poll().has_value());
    const auto power = monitor.Diagnostics();
    APW_EXPECT(power.running);
    APW_EXPECT(power.suspend_events == 2U);
    APW_EXPECT(power.resume_events == 2U);
    monitor.Stop();
    APW_EXPECT(!monitor.Diagnostics().running);

    const auto mutex_name = L"Local\\AirPlayWin.Test." +
                            std::to_wstring(GetCurrentProcessId()) + L"." +
                            std::to_wstring(GetTickCount64());
    {
        SingleInstanceGuard first{mutex_name};
        SingleInstanceGuard second{mutex_name};
        APW_EXPECT(first.Acquired());
        APW_EXPECT(!second.Acquired());
        APW_EXPECT(second.AlreadyRunning());
    }
    SingleInstanceGuard after_release{mutex_name};
    APW_EXPECT(after_release.Acquired());

    const auto executable = WindowsFirewallManager::CurrentExecutablePath();
    APW_EXPECT(!executable.empty());
    const auto specs = WindowsFirewallManager::CoreRuleSpecs(executable, 5'000U, 7'000U);
    APW_EXPECT(specs.size() == 2U);
    APW_EXPECT(specs[0].IsValid() && specs[1].IsValid());
    APW_EXPECT(specs[0].local_ports == L"5000,7000");
    const auto status = WindowsFirewallManager::Query(specs[0]);
    if (status.health == FirewallRuleHealth::Error) {
        APW_EXPECT(status.last_error != 0U);
    }
    const FirewallRuleSpec invalid{};
    const auto invalid_status = WindowsFirewallManager::Query(invalid);
    APW_EXPECT(invalid_status.health == FirewallRuleHealth::Error);
    APW_EXPECT(invalid_status.last_error == ERROR_INVALID_PARAMETER);
}
