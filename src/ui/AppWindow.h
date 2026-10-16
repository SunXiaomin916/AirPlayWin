#pragma once

#include <Windows.h>

#include <filesystem>
#include <memory>
#include <string>

#include "app/ReceiverHost.h"
#include "app/SettingsStore.h"

namespace airplaywin::ui {

using airplaywin::app::AppSettings;

// The graphical application shell: a native Win32 window hosting a WebView2
// UI, a tray icon, settings persistence, and the receiver host. UI and
// core stay separated: the web layer only ever talks to this class through
// JSON messages, and every receiver feature is driven by the same
// ReceiverHost used by the console.
class AppWindow final {
public:
    AppWindow(HINSTANCE instance, AppSettings settings,
              std::filesystem::path ui_directory,
              std::filesystem::path log_directory, bool test_bridge = false);
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    [[nodiscard]] bool Initialize(int show_command) noexcept;
    [[nodiscard]] int Run() noexcept;

    // Management mode helpers used by the entry point before the window
    // exists (single instance activation, elevated firewall requests).
    void ShowWindowFromTray() noexcept;
    void RequestExit() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace airplaywin::ui
