#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "GeneratedBackend.hpp"
#include "i18n.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>

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

namespace {

constexpr wchar_t kAutostartKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAutostartEntry[] = L"Shuttle";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 0x5114;

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

// FileOpenPicker and FolderPicker are WinRT activation-based; unpackaged
// desktop apps must hand them the top-level HWND. Declared locally with
// the canonical IInitializeWithWindow IID: same GUID, same vtable shape,
// immune to SDK header declaration drift.
struct __declspec(uuid("3D4BEFD3-9DFA-4DAF-A44A-2C2E26F1E23A"))
    PickerWindowInitializer : ::IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetWindow(HWND hwnd) = 0;
};

template <typename Picker>
void BindPickerToWindow(Picker const& picker, HWND window) {
  if (auto const target = picker.template try_as<PickerWindowInitializer>()) {
    winrt::check_hresult(target->SetWindow(window));
  }
}

// i18n strings are UTF-8 string_views from the embedded table; project
// text APIs need UTF-16 hstrings.
hstring tr_hstring(char const* key) {
  return winrt::to_hstring(std::string(rivet_app::i18n::tr("zh", key)));
}

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

  DropTitle().Text(tr_hstring("drop.title"));
  DropSubtitle().Text(tr_hstring("drop.subtitle"));
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
                      hstring{L"本机 " +
                              std::wstring{winrt::to_hstring(
                                  settings.machine)}});
                  window->SetupPolling();
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
  ErrorBar().IsOpen(true);
}

void MainWindow::SetupPolling() {
  poll_timer_ = DispatcherTimer();
  poll_timer_.Interval(std::chrono::milliseconds{1500});
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
                message, stamp);
    items_.emplace(id, projected);
    auto const rows =
        transfers_list_.ItemsSource()
            .as<IObservableVector<winrt::RivetHost::TransferItem>>();
    rows.Append(projected);

    if (transfer.direction == rivet_app::Direction::receive &&
        transfer.status == rivet_app::TransferStatus::received &&
        tray_ != nullptr) {
      tray_->Notify(std::wstring{tr_hstring("notify.received-title")},
                    std::wstring{winrt::to_hstring(transfer.filename)});
    }
  } else {
    auto self = winrt::get_self<TransferItem>(existing->second);
    self->Apply(direction, filename, transfer.size, transfer.progress, status,
                message, stamp);
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

  auto const weak = get_weak();
  co_await winrt::resume_background();
  std::vector<hstring> failures;
  for (auto const& path : paths) {
    try {
      rivet_app::API api(*backend);
      (void)api.send_file(winrt::to_string(path)).get();
    } catch (std::exception const& e) {
      failures.push_back(winrt::to_hstring(e.what()));
    }
  }
  if (auto window = weak.get()) {
    window->DispatcherQueue().TryEnqueue(
        [weak, failures = std::move(failures)] {
          if (auto current = weak.get(); !failures.empty()) {
            hstring message = tr_hstring("error.send") + L"\n" + failures.front();
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
  try {
    rivet_app::API api(*backend);
    settings = api.get_settings().get();
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
  ToggleSwitch autostart_toggle;
  Button relay_browse;
  Button download_browse;

  relay_box.Text(hstring{winrt::to_hstring(settings.relay_dir)});
  download_box.Text(hstring{winrt::to_hstring(settings.download_dir)});
  autostart_toggle.OnContent(box_value(hstring(L"")));
  autostart_toggle.OffContent(box_value(hstring(L"")));
  autostart_toggle.IsOn(AutostartEnabled());

  relay_browse.Content(box_value(hstring(L"…")));
  download_browse.Content(box_value(hstring(L"…")));

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

  auto browse_folder = [weak](TextBox box) -> winrt::fire_and_forget {
    try {
      FolderPicker picker;
      picker.FileTypeFilter().Append(L"*");
      BindPickerToWindow(picker, weak.get()->WindowHandle());
      auto const folder = co_await picker.PickSingleFolderAsync();
      if (folder != nullptr) {
        box.Text(folder.Path());
      }
    } catch (...) {
    }
  };
  relay_browse.Click([weak, relay_box, browse_folder](auto const&,
                                                      auto const&) {
    if (weak.get()) {
      browse_folder(relay_box);
    }
  });
  download_browse.Click([weak, download_box, browse_folder](auto const&,
                                                            auto const&) {
    if (weak.get()) {
      browse_folder(download_box);
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
  operation.Completed([weak, backend, relay_box, download_box,
                       autostart_toggle](auto const& async, auto const&) {
    auto const result = async.GetResults();
    if (auto window = weak.get()) {
      window->settings_dialog_ = nullptr;
    }
    if (result != ContentDialogResult::Primary) {
      return;
    }
    try {
      rivet_app::API api(*backend);
      (void)api
          .set_settings(winrt::to_string(relay_box.Text()),
                        winrt::to_string(download_box.Text()))
          .get();
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
      FileOpenPicker picker;
      picker.ViewMode(PickerViewMode::List);
      picker.FileTypeFilter().Append(L"*");
      BindPickerToWindow(picker, window->WindowHandle());
      auto const files = co_await picker.PickMultipleFilesAsync();
      if (files.Size() == 0) co_return;
      std::vector<std::wstring> paths;
      for (auto const& file : files) {
        paths.push_back(std::wstring{file.Path()});
      }
      window->SendPathsAsync(std::move(paths));
    } catch (...) {
    }
  };
  pick();
}

}  // namespace winrt::RivetHost::implementation
