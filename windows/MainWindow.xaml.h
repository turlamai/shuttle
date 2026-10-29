#pragma once

#include "pch.h"
#include "TransferItem.h"
#include "MainWindow.g.h"
#include "GeneratedBackend.hpp"
#include "system_services.hpp"
#include "winui_window.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace winrt::RivetHost::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();

  // Stable HWND access for platform-owned features such as global hotkeys,
  // clipboard listeners, window subclassing, topmost policy, and tray icons.
  [[nodiscard]] HWND WindowHandle() {
    return rivet::system::NativeWindowHandle(*this);
  }

  // Thread-safe orderly backend stop shared by the window-close path and
  // the console-control shutdown hook (logoff/CTRL events).
  void StopBackendOrderly();

  void DropZone_Tapped(winrt::Windows::Foundation::IInspectable const& sender,
                       winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& args);

 private:
  winrt::fire_and_forget InitializeBackendAsync();
  void SetErrorUi(std::string const& message);

  void SetupLocalization();
  void SetupWindowBehavior();
  void SetupTray();
  void UpdateCaptionInset();
  void ApplyRelayHealth(bool ok, std::wstring const& message);
  void UpsertTransfer(rivet_app::Transfer const& transfer);
  void UpdateEmptyState();
  winrt::fire_and_forget SendPathsAsync(std::vector<std::wstring> paths);
  void ShowError(winrt::hstring const& message);
  void ShowSettingsDialog();

  std::shared_ptr<rivet::windows::Backend> backend_;
  std::unique_ptr<rivet::system::TrayController> tray_;

  winrt::Microsoft::UI::Xaml::Controls::Grid root_grid_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::InfoBar error_bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ListView transfers_list_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock empty_state_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock caption_machine_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock health_text_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse health_dot_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border drop_zone_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog settings_dialog_{nullptr};

  std::map<std::wstring, winrt::RivetHost::TransferItem, std::less<>> items_;

  // Window lifecycle: closing hides to tray; a real exit comes from the
  // tray menu and sets exit_requested_ before closing again.
  bool exit_requested_ = false;
  bool tray_available_ = false;
};

}  // namespace winrt::RivetHost::implementation

namespace winrt::RivetHost::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::RivetHost::factory_implementation
