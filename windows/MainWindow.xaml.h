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

// Host helpers shared with MainWindow.Update.cpp (the taskly family
// pattern): declared here, defined in MainWindow.xaml.cpp.
std::filesystem::path executable_path();
winrt::hstring tr_hstring(char const* key);
bool IsMsiInstall();

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
  void SetupPolling();
  void PollTransfers();
  void UpdateDropTitle();
  void ApplyLanguageRefresh();
 public:
  void TransfersList_ItemClick(
      winrt::Windows::Foundation::IInspectable const& sender,
      winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args);

 private:
  void SetUpdateTimer(
      winrt::Microsoft::UI::Xaml::DispatcherTimer timer);
  void StartUpdateTimer();
  void StopUpdateTimer();
  void RunUpdateFlow(winrt::Microsoft::UI::Xaml::Controls::Button update_button,
                     winrt::Microsoft::UI::Xaml::Controls::TextBlock update_status,
                     winrt::Microsoft::UI::Xaml::Controls::Button update_cancel);
  void PollUpdatePhase(winrt::Microsoft::UI::Xaml::Controls::Button update_button,
                       winrt::Microsoft::UI::Xaml::Controls::TextBlock update_status,
                       winrt::Microsoft::UI::Xaml::Controls::Button update_cancel);
  void InstallVerifiedUpdate();
  void LaunchPortableHandoff(std::filesystem::path const& zip);
  void HandleInstallMarkers();
  void ShowInstallFailure(std::wstring const& reason);
  void OpenReleasesPage();
  winrt::fire_and_forget StartSilentUpdateCheck();
  void ShowUpdateOfferDialog(winrt::hstring const& version);
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
  winrt::Microsoft::UI::Xaml::Controls::ComboBox target_box_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog settings_dialog_{nullptr};
  // The silent-check offer dialog; null while no offer is on screen so a
  // background check never stacks a second one over the settings dialog.
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog update_offer_dialog_{nullptr};

  // Background threads cannot emit Rivet events (the emitter lives on
  // the RPC thread), so the list is refreshed by polling instead.
  winrt::Microsoft::UI::Xaml::DispatcherTimer poll_timer_{nullptr};
  // Owned here so the settings dialog's download poller survives while
  // its capturing lambdas stay alive.
  winrt::Microsoft::UI::Xaml::DispatcherTimer update_timer_{nullptr};
  // Update state machine: once a verified artifact is on disk the
  // button turns into "install and restart".
  bool update_install_armed_ = false;
  // The silent-check offer dialog owns its own poll timer and the same
  // offer → download → armed walk as the settings row; state lives here
  // because the dialog outlives ShowUpdateOfferDialog's stack frame.
  winrt::Microsoft::UI::Xaml::DispatcherTimer update_offer_timer_{nullptr};
  bool update_offer_downloading_ = false;
  bool update_offer_armed_ = false;
  // Leaf name of the download directory, shown in receive toasts.
  std::wstring saved_leaf_ = L"Shuttle";
  // Language picked in the settings dialog, applied on save.
  std::wstring pending_ui_language_ = L"auto";

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
