#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

#include <mutex>

namespace winrt::RivetHost::implementation {

// Named after rivet.rktd's identifier (the same identity the updater and
// the MSI handoff use). `Local\` scopes ownership to one logon session —
// right for a per-user/per-machine desktop app.
constexpr wchar_t kInstanceMutexName[] = L"Local\\dev.rivet.shuttle";

App::App() {
  InitializeComponent();

#if defined(_DEBUG) && !defined(DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION)
  UnhandledException([](winrt::Windows::Foundation::IInspectable const&,
                        Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& e) {
    if (::IsDebuggerPresent()) {
      auto const message = e.Message();
      (void)message;
      __debugbreak();
    }
  });
#endif
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
  // Two instances would run two backends racing for the same mailbox
  // (double-consuming transfers, fighting the sweep). Own the named
  // mutex or hand the launch over to the running instance and exit. The
  // OS releases the mutex when this process exits, so the update
  // handoff's exit-then-relaunch flow stays deadlock-free.
  instance_mutex_ = winrt::handle{::CreateMutexW(nullptr, TRUE,
                                                 kInstanceMutexName)};
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    instance_mutex_.close();
    // Best effort: surface the already-running window for the user.
    if (auto const existing = ::FindWindowW(nullptr, L"Shuttle")) {
      if (::IsIconic(existing)) {
        (void)::ShowWindow(existing, SW_RESTORE);
      }
      (void)::SetForegroundWindow(existing);
    }
    winrt::Microsoft::UI::Xaml::Application::Current().Exit();
    return;
  }

  window_ = winrt::make<MainWindow>();
  window_.Activate();
}

}  // namespace winrt::RivetHost::implementation
