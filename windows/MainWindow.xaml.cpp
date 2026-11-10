#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "GeneratedBackend.hpp"
#include "update_handoff.hpp"
#include "i18n.hpp"
#include "i18n-language.h"

#include <atomic>
#include <cstdint>
#include <stdexcept>

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>

#include <objidl.h>
#include <shobjidl_core.h>
#include <winreg.h>

namespace winrt::RivetHost::implementation {

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;

namespace DataTransfer = winrt::Windows::ApplicationModel::DataTransfer;

// The WinAppSDK pickers are the unpackaged-desktop replacement for the
// legacy Windows.Storage pickers (whose UWP activation fails without
// package identity).
namespace Pickers = winrt::Microsoft::Windows::Storage::Pickers;

// Host helpers shared with MainWindow.Update.cpp (the taskly family
// pattern): declared in MainWindow.xaml.h, defined here.
std::filesystem::path executable_path() {
  std::wstring buffer(32768, L'\0');
  auto const length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0 || length == buffer.size()) {
    throw std::runtime_error("GetModuleFileNameW failed");
  }
  buffer.resize(length);
  return std::filesystem::path(buffer);
}

// i18n strings are UTF-8 string_views from the embedded table; project
// text APIs need UTF-16 hstrings. Language follows the shared
// override (Settings) or the system UI language; the embedded table
// carries zh/en and the loader falls back active -> other -> key.
hstring tr_hstring(char const* key) {
  return winrt::to_hstring(
      std::string(rivet_app::i18n::tr(rivet_app::i18n::ActiveLanguage().c_str(),
                                      key)));
}

// MSI-installed copies receive online updates via MsiHandoff; portable
// copies update through the cmd swap handoff. The registry lookup tells
// the shapes apart (docs/UPDATE.md).
bool IsMsiInstall() {
  // Per-machine MSIs land under HKLM (both views); per-user MSIs under
  // HKCU. All three can carry a Shuttle entry.
  const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_LOCAL_MACHINE,
                        HKEY_CURRENT_USER};
  const wchar_t* paths[] = {
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
      L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
  };
  for (int i = 0; i < 3; ++i) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(roots[i], paths[i], 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
      continue;
    }
    for (DWORD i = 0;; ++i) {
      wchar_t name[256];
      DWORD len = 256;
      if (::RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS) {
        break;
      }
      HKEY sub = nullptr;
      if (::RegOpenKeyExW(key, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) {
        continue;
      }
      wchar_t display[256] = {};
      DWORD type = 0;
      DWORD bytes = sizeof(display);
      bool hit =
          ::RegQueryValueExW(sub, L"DisplayName", nullptr, &type,
                             reinterpret_cast<BYTE*>(display), &bytes) ==
              ERROR_SUCCESS &&
          type == REG_SZ && ::wcscmp(display, L"Shuttle") == 0;
      ::RegCloseKey(sub);
      if (hit) {
        ::RegCloseKey(key);
        return true;
      }
    }
    ::RegCloseKey(key);
  }
  return false;
}

namespace {

constexpr wchar_t kAutostartKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAutostartEntry[] = L"Shuttle";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 0x5114;

std::string utf8(std::filesystem::path const& path) {
  auto const wide = path.wstring();
  if (wide.empty()) {
    return {};
  }
  auto const size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          wide.data(),
                                          static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    throw std::runtime_error("WideCharToMultiByte failed");
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                            wide.data(), static_cast<int>(wide.size()),
                            result.data(), size, nullptr, nullptr) != size) {
    throw std::runtime_error("WideCharToMultiByte failed");
  }
  return result;
}

rivet::windows::RacketRuntimeConfig runtime_config() {
  auto const exe = executable_path();
  auto const root = exe.parent_path();
  auto const runtime = root / L"runtime";

  rivet::windows::RacketRuntimeConfig config;
  config.executable_path = utf8(exe);
  config.petite_boot = utf8(runtime / L"petite.boot");
  config.scheme_boot = utf8(runtime / L"scheme.boot");
  config.racket_boot = utf8(runtime / L"racket.boot");
  config.backend_bundle = utf8(root / L"res" / L"core.zo");
  config.module_name = rivet_app::kModuleName;
  config.entry_symbol = rivet_app::kEntryName;
  config.dll_dir = runtime.wstring();
  return config;
}

// i18n strings are UTF-8 string_views from the embedded table; project
// text APIs need UTF-16 hstrings. Language follows the shared
// override (Settings) or the system UI language; the embedded table
// carries zh/en and the loader falls back active -> other -> key.
// (tr_hstring itself is defined at namespace scope above.)

bool AutostartEnabled() {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kAutostartKeyPath, 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD type = 0;
  bool const present =
      ::RegQueryValueExW(key, kAutostartEntry, nullptr, &type, nullptr,
                         nullptr) == ERROR_SUCCESS &&
      type == REG_SZ;
  ::RegCloseKey(key);
  return present;
}

void SetAutostart(bool enable) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kAutostartKeyPath, 0,
                      KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    throw std::runtime_error("RegOpenKeyExW failed");
  }
  if (enable) {
    auto const exe = executable_path().wstring();
    ::RegSetValueExW(key, kAutostartEntry, 0, REG_SZ,
                     reinterpret_cast<BYTE const*>(exe.c_str()),
                     static_cast<DWORD>((exe.size() + 1) * sizeof(wchar_t)));
  } else {
    ::RegDeleteValueW(key, kAutostartEntry);
  }
  ::RegCloseKey(key);
}

Media::SolidColorBrush AccentBrush() {
  Media::SolidColorBrush brush{
      winrt::Windows::UI::Color{0xFF, 0x00, 0x78, 0xD4}};
  return brush;
}

// The active drag state is a plain accent tint; the rest fill is the
// XAML resource on the window's root grid, resolved by the caller
// through the window (the resource moved off Application resources).
Media::SolidColorBrush AccentFillBrush() {
  Media::SolidColorBrush brush{
      winrt::Windows::UI::Color{0x1A, 0x00, 0x78, 0xD4}};
  return brush;
}

Media::SolidColorBrush HealthBrush(bool ok) {
  return Media::SolidColorBrush(
      ok ? winrt::Windows::UI::Color{0xFF, 0x2E, 0xB4, 0x48}
         : winrt::Windows::UI::Color{0xFF, 0xE4, 0x3C, 0x3C});
}

}  // namespace

MainWindow::MainWindow() {
  InitializeComponent();

  Title(L"Shuttle");
  ExtendsContentIntoTitleBar(true);
  SetTitleBar(CaptionBar());

  // The system Min/Max/Close buttons own the top-right once content
  // extends into the title bar; blend their chrome into the backdrop.
  // The caption bar carries identity only — the settings entry lives in
  // the footer status row, clear of the system buttons entirely.
  {
    auto const transparent = winrt::Windows::UI::Color{0, 0, 0, 0};
    auto const title_bar = AppWindow().TitleBar();
    title_bar.ButtonBackgroundColor(transparent);
    title_bar.ButtonInactiveBackgroundColor(transparent);
  }

  // Compact utility window: narrow column, room for ~8 transfer rows.
  // AppWindow.Resize takes physical pixels; scale the logical 460x660.
  {
    auto const app_window = AppWindow();
    double const scale =
        static_cast<double>(::GetDpiForWindow(WindowHandle())) / 96.0;
    auto const width = static_cast<std::int32_t>(460 * scale);
    auto const height = static_cast<std::int32_t>(660 * scale);
    app_window.Resize({width, height});
  }

  if (HICON const icon =
          ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1))) {
    AppWindow().SetIcon(winrt::Microsoft::UI::IconId{
        static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(icon))});
  }

  try {
    SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::MicaBackdrop());
  } catch (...) {
    // Mica is presentation sugar; older builds still run without it.
  }

  SetupLocalization();
  SetupWindowBehavior();
  SetupTray();

  // Ordinary window close and OS session end (logoff/shutdown) both funnel
  // into the same orderly backend stop. The console-control callback runs
  // on its own thread, so it only touches the thread-safe stop() — never
  // XAML state.
  Closed([this](auto const&, auto const&) {
    tray_.reset();
    StopBackendOrderly();
  });
  try {
    rivet::system::InstallShutdownHook([this] { StopBackendOrderly(); });
  } catch (std::exception const&) {
    // Signal coverage is platform policy and never blocks startup; the
    // window-close path above still runs the orderly shutdown.
  }
  InitializeBackendAsync();
}

void MainWindow::SetupLocalization() {
  root_grid_ = RootGrid();
  error_bar_ = ErrorBar();
  transfers_list_ = TransfersList();
  empty_state_ = EmptyState();
  caption_machine_ = CaptionMachine();
  health_text_ = HealthText();
  health_dot_ = HealthDot();
  drop_zone_ = DropZone();
  target_box_ = TargetBox();

  DropTitle().Text(tr_hstring("drop.title.empty"));
  DropSubtitle().Text(tr_hstring("drop.subtitle"));
  UpdateDropTitle();
  TransfersTitle().Text(tr_hstring("transfers.title"));
  EmptyState().Text(tr_hstring("transfers.empty"));
  ClearHistoryButton().Content(box_value(tr_hstring("transfers.clear")));

  auto const rows = single_threaded_observable_vector<winrt::RivetHost::TransferItem>();
  transfers_list_.ItemsSource(rows);
  UpdateEmptyState();

  ClearHistoryButton().Click([weak = get_weak()](auto const&, auto const&) {
    auto window = weak.get();
    if (window == nullptr) return;
    auto const backend = window->backend_;
    if (backend == nullptr || !backend->running()) {
      window->ShowError(tr_hstring("error.backend-not-ready"));
      return;
    }
    try {
      rivet_app::API api(*backend);
      (void)api.clear_history().get();
    } catch (...) {
      window->ShowError(tr_hstring("error.backend-not-ready"));
      return;
    }
    window->items_.clear();
    auto const fresh = single_threaded_observable_vector<winrt::RivetHost::TransferItem>();
    window->transfers_list_.ItemsSource(fresh);
    window->UpdateEmptyState();
  });

  SettingsButton().Click([weak = get_weak()](auto const&, auto const&) {
    if (auto window = weak.get()) {
      window->ShowSettingsDialog();
    }
  });

  // WinUI 3 ComboBox has no TextChanged; drop title refreshes when a
  // known peer is picked, on settings load, and on target edits made
  // through the Settings dialog.
  TargetBox().SelectionChanged([weak = get_weak()](auto const&,
                                                   auto const&) {
    if (auto window = weak.get()) {
      window->UpdateDropTitle();
    }
  });

  DropZone().DragOver([weak = get_weak()](IInspectable const& sender,
                                          DragEventArgs const& args) {
    args.AcceptedOperation(DataTransfer::DataPackageOperation::Copy);
    sender.as<Border>().Background(AccentFillBrush());
    sender.as<Border>().BorderBrush(AccentBrush());
  });

  DropZone().DragLeave([weak = get_weak()](IInspectable const& sender,
                                           DragEventArgs const&) {
    auto window = weak.get();
    if (window == nullptr) return;
    sender.as<Border>().Background(
        window->root_grid_.Resources()
            .Lookup(box_value(hstring(L"DropZoneFill")))
            .as<Media::Brush>());
    sender.as<Border>().BorderBrush(nullptr);
  });

  DropZone().Drop([weak = get_weak()](IInspectable const& sender,
                                      DragEventArgs const& args) {
    {
      auto window = weak.get();
      if (window != nullptr) {
        sender.as<Border>().Background(
            window->root_grid_.Resources()
                .Lookup(box_value(hstring(L"DropZoneFill")))
                .as<Media::Brush>());
        sender.as<Border>().BorderBrush(nullptr);
      }
    }
    if (!args.DataView().Contains(
            DataTransfer::StandardDataFormats::StorageItems())) {
      return;
    }
    auto const operation = args.DataView().GetStorageItemsAsync();
    operation.Completed([weak](auto const& pending, auto const&) {
      try {
        auto const items = pending.GetResults();
        std::vector<std::wstring> paths;
        for (auto const& item : items) {
          paths.push_back(std::wstring{item.Path()});
        }
        if (auto window = weak.get()) {
          window->DispatcherQueue().TryEnqueue(
              [weak, paths = std::move(paths)] {
                if (auto current = weak.get()) {
                  current->SendPathsAsync(std::move(paths));
                }
              });
        }
      } catch (...) {
      }
    });
  });
}

void MainWindow::SetupWindowBehavior() {
  auto const app_window = AppWindow();
  app_window.Closing(
      [this, app_window](
          winrt::Microsoft::UI::Windowing::AppWindow const&,
          winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const&
              args) {
        if (exit_requested_) return;
        if (tray_available_) {
          args.Cancel(true);
          app_window.Hide();
        }
      });
}

void MainWindow::SetupTray() {
  try {
    HICON const icon =
        ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (icon == nullptr) return;

    std::vector<rivet::system::TrayMenuItem> menu;
    menu.emplace_back(static_cast<UINT>(1),
                      std::wstring{tr_hstring("tray.open")},
                      [weak = get_weak()] {
                        if (auto window = weak.get()) {
                          window->AppWindow().Show();
                          window->Activate();
                        }
                      });
    menu.emplace_back(rivet::system::TrayMenuItem::Type::separator);
    menu.emplace_back(static_cast<UINT>(2),
                      std::wstring{tr_hstring("tray.exit")},
                      [weak = get_weak()] {
                        if (auto window = weak.get()) {
                          window->exit_requested_ = true;
                          window->Close();
                        }
                      });

    tray_ = std::make_unique<rivet::system::TrayController>(
        WindowHandle(), kTrayIconId, kTrayCallbackMessage,
        std::wstring{tr_hstring("tray.tooltip")}, icon, std::move(menu));
    tray_available_ = true;
  } catch (...) {
    // Tray presence is an enhancement; the window works without it.
    tray_available_ = false;
  }
}

void MainWindow::StopBackendOrderly() {
  if (auto backend = std::atomic_load(&backend_); backend != nullptr) {
    backend->stop();
  }
}

winrt::fire_and_forget MainWindow::InitializeBackendAsync() {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = std::make_shared<rivet::windows::Backend>(runtime_config());

  try {
    // Booting the embedded runtime can block on file I/O, so only startup is
    // moved off the UI thread. RPC/State traffic below is completion-driven.
    co_await winrt::resume_background();
    backend->start();

    dispatcher.TryEnqueue([weak, backend = std::move(backend)]() mutable {
      if (auto window = weak.get()) {
        window->backend_ = std::move(backend);
        try {
          auto const live = window->backend_;

          // Live events: decode on the reader thread, marshal to the UI.
          live->set_event_handler([weak](std::string const& name,
                                         rivet::Value const& value) {
            rivet_app::Event const event = rivet_app::decode_event(name, value);
            std::visit(
                [&](auto const& payload) {
                  using Payload = std::decay_t<decltype(payload)>;
                  if constexpr (std::is_same_v<
                                    Payload,
                                    rivet_app::Transfer_changedEvent>) {
                    auto const transfer = payload.value;
                    if (auto current = weak.get()) {
                      current->DispatcherQueue().TryEnqueue([weak, transfer] {
                        if (auto window = weak.get()) {
                          window->UpsertTransfer(transfer);
                        }
                      });
                    }
                  } else {
                    auto const health = payload.value;
                    if (auto current = weak.get()) {
                      current->DispatcherQueue().TryEnqueue(
                          [weak, health] {
                            if (auto window = weak.get()) {
                              window->ApplyRelayHealth(
                                  health.ok,
                                  std::wstring{
                                      winrt::to_hstring(health.message)});
                            }
                          });
                    }
                  }
                },
                event);
          });

          rivet_app::API api(*live);
          auto const initial = api.list_transfers().get();
          auto const settings = api.get_settings().get();

          window->DispatcherQueue().TryEnqueue(
              [weak, initial = std::move(initial), settings] {
                if (auto window = weak.get()) {
                  for (auto const& transfer : initial) {
                    window->UpsertTransfer(transfer);
                  }
                  window->caption_machine_.Text(
                      hstring{
                          std::wstring{tr_hstring("caption.machine")} +
                          L" " +
                          std::wstring{winrt::to_hstring(
                              settings.machine)}});
                  try {
                    if (auto flavor_backend = window->backend_) {
                      rivet_app::API flavor_api(*flavor_backend);
                      (void)flavor_api.set_install_flavor_async(
                          IsMsiInstall() ? "msi" : "portable",
                          [](rivet_app::Result<bool>) {});
                    }
                  } catch (...) {
                  }
                  window->saved_leaf_ =
                      std::filesystem::path{
                          std::wstring{winrt::to_hstring(
                              settings.download_dir)}}
                          .filename()
                          .c_str();
                  if (!settings.target.empty()) {
                    window->target_box_.Text(
                        hstring{winrt::to_hstring(settings.target)});
                  }
                  try {
                    if (auto peers_backend = window->backend_) {
                      rivet_app::API peers_api(*peers_backend);
                      auto const peers = peers_api.list_known_peers().get();
                      window->DispatcherQueue().TryEnqueue(
                          [weak, peers] {
                            if (auto w = weak.get()) {
                              for (auto const& peer : peers) {
                                w->target_box_.Items().Append(
                                    box_value(
                                        hstring{winrt::to_hstring(peer)}));
                              }
                            }
                          });
                    }
                  } catch (...) {
                  }
                  // The backend is answering: clear any transient boot
                  // banner ("Backend is not ready yet" was sticky and kept
                  // implying failure long after startup succeeded).
                  window->ErrorBar().IsOpen(false);
                  window->SetupPolling();
                  window->HandleInstallMarkers();
                  window->StartSilentUpdateCheck();
                }
              });
        } catch (std::exception const& e) {
          window->SetErrorUi(e.what());
        }
      } else {
        // Never destroy the last Backend reference on its own reader thread.
        std::thread([backend = std::move(backend)]() mutable {
          backend->stop();
        }).detach();
      }
    });
  } catch (std::exception const& e) {
    auto message = std::string(e.what());
    dispatcher.TryEnqueue([weak, message = std::move(message)] {
      if (auto window = weak.get()) {
        window->SetErrorUi(message);
      }
    });
  }
}

void MainWindow::SetErrorUi(std::string const& message) {
  ErrorBar().Severity(InfoBarSeverity::Error);
  ErrorBar().Message(winrt::to_hstring(message));
  ErrorBar().IsOpen(true);
}

void MainWindow::ShowError(hstring const& message) {
  ErrorBar().Severity(InfoBarSeverity::Error);
  ErrorBar().Message(message);
  // The copy action turns every error into a bug-report start (the
  // taskly updateCopyError contract): one click puts the verbatim
  // message on the clipboard.
  auto copy = Button();
  copy.Content(box_value(tr_hstring("error.copy")));
  copy.Click([message](auto const&, auto const&) {
    try {
      DataTransfer::DataPackage package;
      package.SetText(message);
      DataTransfer::Clipboard::SetContent(package);
    } catch (...) {
    }
  });
  ErrorBar().ActionButton(copy);
  ErrorBar().IsOpen(true);
}

void MainWindow::SetupPolling() {
  poll_timer_ = DispatcherTimer();
  // Live updates arrive as backend events (worker-thread emission is
  // supported since rivet #364); this slow timer only reconciles —
  // history clears, a missed event, clock drift.
  poll_timer_.Interval(std::chrono::milliseconds{10000});
  poll_timer_.Tick([weak = get_weak()](auto const&, auto const&) {
    if (auto window = weak.get()) {
      window->PollTransfers();
    }
  });
  poll_timer_.Start();
}

// Poll the authoritative transfer table and reconcile the list. The
// RPC reads in-memory state (no share I/O), so the round trip is fast.
void MainWindow::PollTransfers() {
  auto const backend = backend_;
  if (backend == nullptr || !backend->running()) return;
  try {
    rivet_app::API api(*backend);
    auto const snapshot = api.list_transfers().get();
    auto const weak = get_weak();
    DispatcherQueue().TryEnqueue([weak, snapshot = std::move(snapshot)] {
      if (auto window = weak.get()) {
        for (auto const& transfer : snapshot) {
          window->UpsertTransfer(transfer);
        }
        // Drop rows whose transfers no longer exist (history cleared).
        std::vector<std::wstring> gone;
        for (auto const& [id, item] : window->items_) {
          bool present = false;
          for (auto const& transfer : snapshot) {
            if (std::wstring{winrt::to_hstring(transfer.id)} == id) {
              present = true;
              break;
            }
          }
          if (!present) gone.push_back(id);
        }
        if (!gone.empty()) {
          auto const rows =
              window->transfers_list_.ItemsSource()
                  .as<Windows::Foundation::Collections::IObservableVector<
                      winrt::RivetHost::TransferItem>>();
          for (auto const& id : gone) {
            auto it = window->items_.find(id);
            if (it == window->items_.end()) continue;
            uint32_t index = 0;
            if (rows.IndexOf(it->second, index)) {
              rows.RemoveAt(index);
            }
            window->items_.erase(id);
          }
          window->UpdateEmptyState();
        }
      }
    });
  } catch (...) {
    // Backend shutting down; the next tick retries.
  }
}

void MainWindow::SetUpdateTimer(
    winrt::Microsoft::UI::Xaml::DispatcherTimer timer) {
  update_timer_ = std::move(timer);
}

void MainWindow::StartUpdateTimer() {
  if (update_timer_) update_timer_.Start();
}

void MainWindow::StopUpdateTimer() {
  if (update_timer_) update_timer_.Stop();
}


void MainWindow::ApplyLanguageRefresh() {
  // Re-render every static text after the language override changes.
  DropTitle().Text(hstring(tr_hstring(target_box_.Text().empty()
                                          ? "drop.title.empty"
                                          : "drop.title.ready")));
  DropSubtitle().Text(tr_hstring("drop.subtitle"));
  TransfersTitle().Text(tr_hstring("transfers.title"));
  EmptyState().Text(tr_hstring("transfers.empty"));
  ClearHistoryButton().Content(box_value(tr_hstring("transfers.clear")));
  UpdateDropTitle();
}

void MainWindow::UpdateDropTitle() {
  // The drop zone states the action with the current recipient inline:
  // no target -> prompt for it; target set -> name it in the title.
  auto const target = target_box_.Text();
  if (target.empty()) {
    DropTitle().Text(tr_hstring("drop.title.empty"));
    DropSubtitle().Text(tr_hstring("drop.subtitle"));
  } else {
    DropTitle().Text(hstring{
        std::wstring{tr_hstring("drop.title.ready")} + L" " + target});
    DropSubtitle().Text(tr_hstring("drop.subtitle"));
  }
}

// Bound from the XAML ItemClick attribute: clicking a completed
// received row opens its containing folder.
void MainWindow::TransfersList_ItemClick(
    winrt::Windows::Foundation::IInspectable const&,
    Controls::ItemClickEventArgs const& args) {
  auto item = args.ClickedItem().try_as<winrt::RivetHost::TransferItem>();
  if (item == nullptr) return;
  if (item.OpenFolderVisibility() != Visibility::Visible) return;

  auto const parent =
      std::filesystem::path{std::wstring{item.LocalPath()}}
          .parent_path()
          .wstring();
  (void)Windows::System::Launcher::LaunchFolderAsync(
      Windows::Storage::StorageFolder::GetFolderFromPathAsync(parent).get());
}

void MainWindow::ApplyRelayHealth(bool ok, std::wstring const& message) {
  HealthDot().Fill(HealthBrush(ok));
  HealthText().Text(hstring(message));
}

void MainWindow::UpsertTransfer(rivet_app::Transfer const& transfer) {
  std::wstring const id = std::wstring{winrt::to_hstring(transfer.id)};
  auto const direction = static_cast<std::int32_t>(transfer.direction);
  auto const status = static_cast<std::int32_t>(transfer.status);
  hstring const filename{winrt::to_hstring(transfer.filename)};
  hstring const message{winrt::to_hstring(transfer.message)};
  hstring const stamp{winrt::to_hstring(transfer.stamp)};

  auto existing = items_.find(id);
  if (existing == items_.end()) {
    winrt::RivetHost::TransferItem projected;
    auto self = winrt::get_self<TransferItem>(projected);
    self->Id(hstring{id});
    self->Apply(direction, filename, transfer.size, transfer.progress, status,
                message, stamp,
                hstring{winrt::to_hstring(transfer.local_path)});
    items_.emplace(id, projected);
    auto const rows =
        transfers_list_.ItemsSource()
            .as<IObservableVector<winrt::RivetHost::TransferItem>>();
    rows.Append(projected);

    if (transfer.direction == rivet_app::Direction::receive &&
        transfer.status == rivet_app::TransferStatus::received &&
        tray_ != nullptr) {
      tray_->Notify(
          std::wstring{tr_hstring("notify.received-title")},
          std::wstring{winrt::to_hstring(transfer.filename)} + L"\n" +
              saved_leaf_);
    }
  } else {
    auto self = winrt::get_self<TransferItem>(existing->second);
    self->Apply(direction, filename, transfer.size, transfer.progress, status,
                message, stamp,
                hstring{winrt::to_hstring(transfer.local_path)});
  }
  UpdateEmptyState();
}

void MainWindow::UpdateEmptyState() {
  auto const empty = items_.empty();
  EmptyState().Visibility(empty ? Visibility::Visible : Visibility::Collapsed);
  TransfersTitle().Visibility(empty ? Visibility::Collapsed : Visibility::Visible);
  ClearHistoryButton().Visibility(empty ? Visibility::Collapsed : Visibility::Visible);
}

winrt::fire_and_forget MainWindow::SendPathsAsync(
    std::vector<std::wstring> paths) {
  auto const backend = backend_;
  if (backend == nullptr || !backend->running()) {
    ShowError(tr_hstring("error.backend-not-ready"));
    co_return;
  }

  // Read WinUI controls before switching to a background apartment.
  auto const target = winrt::to_string(TargetBox().Text());
  auto const weak = get_weak();
  co_await winrt::resume_background();
  std::vector<hstring> failures;
  std::size_t folders = 0;
  for (auto const& path : paths) {
    // Explorer drops include folders. The mailbox protocol transfers
    // one file per item; a folder can only die deep inside the backend
    // with a misleading "file not found", so intercept it here with the
    // user-facing remedy instead.
    std::error_code ec;
    if (std::filesystem::is_directory(
            std::filesystem::path{path.c_str()}, ec)) {
      ++folders;
      continue;
    }
    try {
      rivet_app::API api(*backend);
      (void)api.send_file(winrt::to_string(path), target).get();
    } catch (std::exception const& e) {
      failures.push_back(winrt::to_hstring(e.what()));
    }
  }
  if (auto window = weak.get()) {
    window->DispatcherQueue().TryEnqueue(
        [weak, failures = std::move(failures), folders] {
          if (auto current = weak.get(); !failures.empty() || folders > 0) {
            hstring message = tr_hstring("error.send");
            if (!failures.empty()) {
              message = message + L"\n" + failures.front();
            }
            if (folders > 0) {
              message = message + L"\n" + tr_hstring("error.send-folder");
            }
            current->ShowError(message);
          }
        });
  }
}

void MainWindow::ShowSettingsDialog() {
  auto const backend = backend_;
  if (backend == nullptr || !backend->running()) {
    ShowError(tr_hstring("error.backend-not-ready"));
    return;
  }

  rivet_app::Settings settings;
  bool auto_check_updates = true;
  try {
    rivet_app::API api(*backend);
    settings = api.get_settings().get();
    auto_check_updates = api.get_auto_check_updates().get();
  } catch (std::exception const& e) {
    SetErrorUi(e.what());
    return;
  }

  auto const weak = get_weak();

  // The dialog must outlive ShowAsync; keep it in a member until it closes.
  auto dialog = ContentDialog();
  settings_dialog_ = dialog;

  TextBox relay_box;
  TextBox download_box;
  TextBox identity_box;
  ComboBox language_box;
  ToggleSwitch autostart_toggle;
  ToggleSwitch auto_check_toggle;
  Button relay_browse;
  Button download_browse;
  Button update_cancel;

  relay_box.Text(hstring{winrt::to_hstring(settings.relay_dir)});
  download_box.Text(hstring{winrt::to_hstring(settings.download_dir)});
  identity_box.Text(hstring{winrt::to_hstring(settings.identity)});
  identity_box.PlaceholderText(hstring{winrt::to_hstring(settings.machine)});
  // interface language: index 0 = auto, 1 = zh, 2 = en
  language_box.Items().Append(
      box_value(hstring{tr_hstring("settings.language-auto")}));
  language_box.Items().Append(box_value(hstring(L"中文")));
  language_box.Items().Append(box_value(hstring(L"English")));
  language_box.SelectedIndex(settings.ui_language == "zh"
                                 ? 1
                                 : (settings.ui_language == "en" ? 2 : 0));
  autostart_toggle.OnContent(box_value(hstring(L"")));
  autostart_toggle.OffContent(box_value(hstring(L"")));
  autostart_toggle.IsOn(AutostartEnabled());
  auto_check_toggle.OnContent(box_value(hstring(L"")));
  auto_check_toggle.OffContent(box_value(hstring(L"")));
  auto_check_toggle.IsOn(auto_check_updates);

  relay_browse.Content(box_value(hstring(L"…")));
  download_browse.Content(box_value(hstring(L"…")));

  // The settings browse buttons open the WinAppSDK FolderPicker: the
  // legacy Windows.Storage pickers depend on UWP activation and fail on
  // unpackaged desktop apps (the failure used to be swallowed, so the
  // button silently did nothing). A picked folder only updates the box;
  // it takes effect through the dialog's normal Save path. Picker
  // failures surface through the error bar instead of vanishing.
  auto wire_folder_browse = [weak](Button button, TextBox box,
                                   char const* automation_key) {
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        button, tr_hstring(automation_key));
    button.Click([weak, box](auto const&, auto const&) ->
                 winrt::fire_and_forget {
      auto window = weak.get();
      if (window == nullptr) co_return;
      try {
        Pickers::FolderPicker picker(window->AppWindow().Id());
        picker.ViewMode(Pickers::PickerViewMode::List);
        auto const folder = co_await picker.PickSingleFolderAsync();
        if (folder != nullptr) {
          box.Text(folder.Path());
        }
      } catch (winrt::hresult_error const& e) {
        window->ShowError(e.message());
      } catch (...) {
        window->ShowError(tr_hstring("error.picker"));
      }
    });
  };
  wire_folder_browse(relay_browse, relay_box, "browse.relay-dir");
  wire_folder_browse(download_browse, download_box, "browse.download-dir");

  auto make_row = [](hstring const& label, TextBox const& box,
                     Button const& browse) {
    auto caption = TextBlock();
    caption.Text(label);
    caption.FontSize(12);
    caption.Opacity(0.75);

    auto row = Grid();
    auto col0 = ColumnDefinition();
    col0.Width(GridLength{1.0, GridUnitType::Star});
    auto col1 = ColumnDefinition();
    col1.Width(GridLength{0.0, GridUnitType::Auto});
    row.ColumnDefinitions().Append(col0);
    row.ColumnDefinitions().Append(col1);
    row.SetColumn(box, 0);
    row.SetColumn(browse, 1);
    row.ColumnSpacing(8);
    row.Children().Append(box);
    row.Children().Append(browse);

    auto stack = StackPanel();
    stack.Spacing(6);
    stack.Children().Append(caption);
    stack.Children().Append(row);
    return stack;
  };

  auto content = StackPanel();
  content.Spacing(16);
  content.Children().Append(make_row(tr_hstring("settings.relay-label"),
                                     relay_box, relay_browse));
  content.Children().Append(make_row(tr_hstring("settings.download-label"),
                                     download_box, download_browse));
  {
    auto caption = TextBlock();
    caption.Text(tr_hstring("settings.autostart"));
    caption.VerticalAlignment(VerticalAlignment::Center);

    auto autostart_row = StackPanel();
    autostart_row.Orientation(Orientation::Horizontal);
    autostart_row.Spacing(8);
    autostart_row.Children().Append(caption);
    autostart_row.Children().Append(autostart_toggle);
    content.Children().Append(autostart_row);
  }

  {
    auto caption = TextBlock();
    caption.Text(tr_hstring("settings.identity-label"));
    caption.FontSize(12);
    caption.Opacity(0.75);
    auto hint = TextBlock();
    hint.Text(tr_hstring("settings.identity-hint"));
    hint.FontSize(11);
    hint.Opacity(0.55);
    hint.TextWrapping(TextWrapping::Wrap);
    auto stack = StackPanel();
    stack.Spacing(6);
    stack.Children().Append(caption);
    stack.Children().Append(identity_box);
    stack.Children().Append(hint);
    content.Children().Append(stack);
  }

  {
    auto caption = TextBlock();
    caption.Text(tr_hstring("settings.language-label"));
    caption.FontSize(12);
    caption.Opacity(0.75);
    auto stack = StackPanel();
    stack.Spacing(6);
    stack.Children().Append(caption);
    stack.Children().Append(language_box);
    content.Children().Append(stack);
  }

  // --- update block: a member-function state machine, not nested
  // lambdas. One button walks check -> download -> install; a cancel
  // button appears only while a download is in flight.
  auto update_row = StackPanel();
  update_row.Orientation(Orientation::Horizontal);
  update_row.Spacing(8);
  auto update_status = TextBlock();
  update_status.Opacity(0.75);
  update_status.VerticalAlignment(VerticalAlignment::Center);
  update_status.FontSize(12);
  update_status.Text(tr_hstring("update.idle"));
  auto update_button = Button();
  update_button.Content(box_value(tr_hstring("update.check")));
  update_cancel.Content(box_value(tr_hstring("update.cancel-download")));
  update_cancel.Visibility(Visibility::Collapsed);
  update_cancel.Click([weak, update_button, update_status,
                       update_cancel](auto const&, auto const&) {
    if (auto window = weak.get()) {
      window->StopUpdateTimer();
      window->update_install_armed_ = false;
      auto const backend = window->backend_;
      if (backend != nullptr && backend->running()) {
        try {
          rivet_app::API api(*backend);
          (void)api.cancel_download().get();
        } catch (...) {
        }
      }
      window->PollUpdatePhase(update_button, update_status, update_cancel);
    }
  });
  update_row.Children().Append(update_status);
  update_row.Children().Append(update_button);
  update_row.Children().Append(update_cancel);
  content.Children().Append(update_row);

  // --- auto-check toggle: gates the silent launch check only; the
  // manual 检查更新 button above ignores it.
  {
    auto auto_check_caption = TextBlock();
    auto_check_caption.Text(tr_hstring("update.auto-check"));
    auto_check_caption.VerticalAlignment(VerticalAlignment::Center);
    auto auto_check_row = StackPanel();
    auto_check_row.Orientation(Orientation::Horizontal);
    auto_check_row.Spacing(8);
    auto_check_row.Children().Append(auto_check_caption);
    auto_check_row.Children().Append(auto_check_toggle);
    content.Children().Append(auto_check_row);
  }

  update_install_armed_ = false;
  update_button.Click([weak = get_weak(), update_button, update_status,
                       update_cancel](auto const&, auto const&) {
    if (auto window = weak.get()) {
      if (window->update_install_armed_) {
        window->InstallVerifiedUpdate();
      } else {
        window->RunUpdateFlow(update_button, update_status, update_cancel);
      }
    }
  });

  dialog.Title(box_value(tr_hstring("settings.title")));
  dialog.Content(content);
  dialog.PrimaryButtonText(tr_hstring("settings.save"));
  dialog.CloseButtonText(tr_hstring("settings.cancel"));
  dialog.DefaultButton(ContentDialogButton::Primary);
  dialog.XamlRoot(Content().XamlRoot());

  // WinUI 3 ContentDialog exposes the dismissal result only through
  // ShowAsync, so the save runs in the operation's completion.
  auto const operation = dialog.ShowAsync();
  operation.Completed([weak, backend, relay_box, download_box, identity_box,
                       language_box, autostart_toggle,
                       auto_check_toggle](auto const& async, auto const&) {
    auto const result = async.GetResults();
    if (auto window = weak.get()) {
      window->settings_dialog_ = nullptr;
    }
    if (result != ContentDialogResult::Primary) {
      return;
    }
    try {
      auto window = weak.get();
      if (!window) return;
      rivet_app::API api(*backend);
      auto const lang_choice =
          language_box.SelectedIndex() == 1
              ? std::string{"zh"}
              : (language_box.SelectedIndex() == 2 ? std::string{"en"}
                                                   : std::string{"auto"});
      rivet_app::i18n::SetOverrideLanguage(lang_choice == "auto"
                                               ? std::string{}
                                               : lang_choice);
      (void)api
          .set_settings(winrt::to_string(relay_box.Text()),
                        winrt::to_string(download_box.Text()),
                        winrt::to_string(identity_box.Text()),
                        winrt::to_string(std::wstring{window->TargetBox().Text()}),
                        lang_choice)
          .get();
      window->ApplyLanguageRefresh();
    } catch (std::exception const& e) {
      if (auto window = weak.get()) {
        window->DispatcherQueue().TryEnqueue(
            [weak, message = winrt::to_hstring(e.what())] {
              if (auto current = weak.get()) {
                current->ShowError(message);
              }
            });
      }
      return;
    }
    try {
      SetAutostart(autostart_toggle.IsOn());
    } catch (...) {
    }
    try {
      rivet_app::API api(*backend);
      (void)api.set_auto_check_updates(auto_check_toggle.IsOn()).get();
    } catch (...) {
    }
  });
}

void MainWindow::DropZone_Tapped(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&) {
  auto const weak = get_weak();
  auto pick = [weak]() -> winrt::fire_and_forget {
    try {
      auto window = weak.get();
      if (window == nullptr) co_return;
      auto const backend = window->backend_;
      if (backend == nullptr || !backend->running()) {
        window->ShowError(tr_hstring("error.backend-not-ready"));
        co_return;
      }
      Pickers::FileOpenPicker picker(window->AppWindow().Id());
      picker.ViewMode(Pickers::PickerViewMode::List);
      auto const files = co_await picker.PickMultipleFilesAsync();
      if (files.Size() == 0) co_return;
      std::vector<std::wstring> paths;
      for (auto const& file : files) {
        paths.push_back(std::wstring{file.Path()});
      }
      window->SendPathsAsync(std::move(paths));
    } catch (winrt::hresult_error const& e) {
      if (auto window = weak.get()) window->ShowError(e.message());
    } catch (...) {
      if (auto window = weak.get()) {
        window->ShowError(tr_hstring("error.picker"));
      }
    }
  };
  pick();
}

}  // namespace winrt::RivetHost::implementation
