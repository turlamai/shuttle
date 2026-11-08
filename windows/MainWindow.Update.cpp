// Online update flow for the Shuttle window (docs/UPDATE.md), the
// taskly/payback family shape: the UI skeleton (silent throttled launch
// check, manual check, 1 s progress polling, failure reporting) lives
// here; install differs by install shape — MSI copies hand off to a
// verified passive msiexec (rivet's MsiHandoff), portable copies hand
// off to a cmd batch that swaps the install tree after process exit.

#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "update_handoff.hpp"

#include <winrt/Windows.System.h>

#include <shellapi.h>

#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace winrt::RivetHost::implementation {

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;

namespace {

// Same identity constant as the backend updater (rivet.rktd's
// identifier); it names the MsiHandoff attempt directories and the
// update temp root.
constexpr wchar_t kUpdateApplicationId[] = L"dev.rivet.shuttle";
constexpr wchar_t kReleasesUrl[] =
    L"https://github.com/turlamai/shuttle/releases/latest";

std::wstring lowercase(std::wstring text) {
  for (auto& c : text) {
    c = static_cast<wchar_t>(::towlower(c));
  }
  return text;
}

// %TEMP%\shuttle-update — handoff script, extract dir, and markers live
// here. Empty on failure; every caller must tolerate that.
std::filesystem::path update_work_dir() {
  wchar_t buffer[MAX_PATH]{};
  std::filesystem::path base;
  auto const length = ::GetEnvironmentVariableW(L"TEMP", buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    base = std::filesystem::path(buffer);
  } else {
    auto const fallback = ::GetEnvironmentVariableW(L"TMP", buffer, MAX_PATH);
    if (fallback > 0 && fallback < MAX_PATH) {
      base = std::filesystem::path(buffer);
    }
  }
  if (base.empty()) {
    return {};
  }
  auto const dir = base / L"shuttle-update";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::filesystem::path update_failure_marker() {
  return update_work_dir() / L"update-failed.txt";
}

std::filesystem::path update_success_marker() {
  return update_work_dir() / L"update-success.txt";
}

// The detached portable handoff batch. ASCII on purpose: cmd parses
// batches in the ANSI code page, so paths travel as quoted %~1…%~5
// arguments instead of living in the script body. The zip is flat (the
// release pipeline packs the stage layout, so RivetHost.exe / res /
// runtime sit at the archive root). Order matters: extract and verify
// BEFORE touching the old install, so a corrupt zip can never break the
// running copy; a failed swap restores the renamed original.
std::string portable_handoff_batch(unsigned long pid) {
  std::string const pid_text = std::to_string(pid);
  std::string batch;
  batch += "@echo off\r\n";
  batch += "rem Shuttle update handoff - auto-generated, safe to delete.\r\n";
  batch += "set /a n=0\r\n";
  batch += ":wait\r\n";
  batch += "tasklist /FI \"PID eq " + pid_text + "\" 2>nul | find \"" +
           pid_text + "\" >nul\r\n";
  batch += "if errorlevel 1 goto extract\r\n";
  batch += "ping -n 2 127.0.0.1 >nul\r\n";
  batch += "set /a n+=1\r\n";
  batch += "if %n% LSS 30 goto wait\r\n";
  // The app never freed itself within 30 s: leave it running, report.
  batch += "> \"%~4\" echo process-exit-timeout\r\n";
  batch += "exit /b 1\r\n";
  batch += ":extract\r\n";
  batch += "set \"extract=%~dp0extract\"\r\n";
  batch += "if exist \"%extract%\" rmdir /s /q \"%extract%\"\r\n";
  batch += "mkdir \"%extract%\" 2>nul\r\n";
  batch += "tar -xf \"%~1\" -C \"%extract%\"\r\n";
  batch += "if errorlevel 1 goto fail_extract\r\n";
  batch += "if exist \"%~2.old\" rmdir /s /q \"%~2.old\"\r\n";
  batch += "move \"%~2\" \"%~2.old\" >nul 2>&1\r\n";
  batch += "if errorlevel 1 goto fail_rename\r\n";
  batch += "robocopy \"%extract%\" \"%~2\" /E /MOVE /NFL /NDL /NJH /NJS /NP >nul\r\n";
  batch += "if errorlevel 8 goto fail_swap\r\n";
  batch += "start \"\" \"%~3\"\r\n";
  batch += "> \"%~5\" echo ok\r\n";
  batch += "exit /b 0\r\n";
  batch += ":fail_swap\r\n";
  batch += "> \"%~4\" echo swap-failed (%errorlevel%)\r\n";
  batch += "move \"%~2.old\" \"%~2\" >nul 2>&1\r\n";
  batch += "goto restart_old\r\n";
  batch += ":fail_rename\r\n";
  batch += "> \"%~4\" echo rename-failed (%errorlevel%)\r\n";
  batch += "goto restart_old\r\n";
  batch += ":fail_extract\r\n";
  batch += "> \"%~4\" echo extract-failed (%errorlevel%)\r\n";
  batch += ":restart_old\r\n";
  batch += "start \"\" \"%~3\"\r\n";
  batch += "exit /b 1\r\n";
  return batch;
}

// ZIP 便携版语义: a copy without the packaged layout (res/ + runtime/
// beside the exe) or one running from a temp path is a dev copy —
// online updates are unavailable there (docs/UPDATE.md).
bool IsDevCopy() {
  try {
    auto const install_dir = executable_path().parent_path();
    std::error_code ec;
    if (!std::filesystem::exists(install_dir / L"res", ec) ||
        !std::filesystem::exists(install_dir / L"runtime", ec)) {
      return true;
    }
    return lowercase(install_dir.wstring()).find(L"temp") !=
           std::wstring::npos;
  } catch (...) {
    return true;
  }
}

}  // namespace

// ---------- check ---------------------------------------------------------

void MainWindow::RunUpdateFlow(
    Controls::Button update_button, Controls::TextBlock update_status,
    Controls::Button update_cancel) {
  auto const backend = backend_;
  if (backend == nullptr || !backend->running()) {
    ShowError(tr_hstring("error.backend-not-ready"));
    return;
  }
  if (IsDevCopy()) {
    ShowError(tr_hstring("update.dev-copy"));
    return;
  }
  auto const set_status = [update_status](hstring const& text) {
    update_status.Text(text);
  };
  auto const set_button = [update_button](hstring const& text, bool enabled) {
    update_button.Content(box_value(text));
    update_button.IsEnabled(enabled);
  };
  set_button(hstring(tr_hstring("update.working")), false);
  set_status(hstring(tr_hstring("update.checking")));

  // Persist the host-detected install shape before asking the backend to
  // select an artifact (the UPDATE.md ordering rule): the fire-and-forget
  // startup stamp can otherwise race this check and make an MSI install
  // download the zip.
  try {
    rivet_app::API api(*backend);
    (void)api.set_install_flavor(IsMsiInstall() ? "msi" : "portable").get();
  } catch (...) {
  }

  auto const weak = get_weak();
  rivet_app::API api(*backend);
  (void)api.check_updates_async(
      [weak, backend, update_button, update_status, update_cancel](
          rivet_app::Result<std::string> result) {
        auto const window = weak.get();
        if (window == nullptr) return;
        window->DispatcherQueue().TryEnqueue(
            [weak, backend, update_button, update_status, update_cancel,
             result]() {
              auto const window2 = weak.get();
              if (window2 == nullptr) return;
              // A failed check (dev copy refuses updates, network down,
              // manifest unverifiable) surfaces here: show the reason and
              // open the releases page for a manual grab.
              auto const set_status =
                  [update_status](hstring const& t) { update_status.Text(t); };
              auto const set_button = [update_button](hstring const& t,
                                                      bool enabled) {
                update_button.Content(box_value(t));
                update_button.IsEnabled(enabled);
              };
              try {
                (void)result.get();
              } catch (std::exception const& check_error) {
                // Portable flavor refuses updates; the network or the
                // manifest may be unreachable. Show why, offer the page.
                set_status(winrt::to_hstring(check_error.what()));
                window2->OpenReleasesPage();
                set_button(hstring(tr_hstring("update.check")), true);
                return;
              }
              window2->PollUpdatePhase(update_button, update_status,
                                       update_cancel);
            });
      });
}

void MainWindow::PollUpdatePhase(
    Controls::Button update_button, Controls::TextBlock update_status,
    Controls::Button update_cancel) {
  auto const backend = backend_;
  if (backend == nullptr || !backend->running()) return;
  auto const set_status = [update_status](hstring const& text) {
    update_status.Text(text);
  };
  auto const set_button = [update_button](hstring const& text, bool enabled) {
    update_button.Content(box_value(text));
    update_button.IsEnabled(enabled);
  };
  auto const set_cancel = [update_cancel](bool visible) {
    update_cancel.Visibility(visible ? Visibility::Visible
                                     : Visibility::Collapsed);
  };
  try {
    rivet_app::API api(*backend);
    auto const phase = api.updater_phase().get();
    if (phase == "available") {
      set_button(hstring(tr_hstring("update.working")), false);
      set_status(hstring(tr_hstring("update.downloading")));
      set_cancel(false);
      (void)api.start_download().get();

      auto weak = get_weak();
      update_timer_ = DispatcherTimer();
      update_timer_.Interval(std::chrono::milliseconds{1000});
      update_timer_.Tick([weak, update_button, update_status,
                          update_cancel](auto const&, auto const&) {
            if (auto window = weak.get()) {
              window->PollUpdatePhase(update_button, update_status,
                                      update_cancel);
            }
          });
      update_timer_.Start();
    } else if (phase == "downloaded") {
      update_install_armed_ = true;
      set_cancel(false);
      if (auto const version = api.updater_available_version().get();
          !version.empty()) {
        set_status(hstring{
            std::wstring{tr_hstring("update.ready")} + L" " +
            std::wstring{winrt::to_hstring(version)}});
      } else {
        set_status(hstring(tr_hstring("update.downloaded")));
      }
      set_button(hstring(tr_hstring("update.install")), true);
      StopUpdateTimer();
    } else if (phase == "error") {
      update_install_armed_ = false;
      StopUpdateTimer();
      set_cancel(false);
      set_button(hstring(tr_hstring("update.check")), true);
      OpenReleasesPage();
    } else if (phase == "up-to-date") {
      update_install_armed_ = false;
      StopUpdateTimer();
      set_cancel(false);
      set_status(hstring(tr_hstring("update.uptodate")));
      set_button(hstring(tr_hstring("update.check")), true);
    } else if (phase == "downloading") {
      set_cancel(true);
      auto const percent = api.updater_percent().get();
      set_status(hstring{
          std::wstring{tr_hstring("update.progress")} + L" " +
          std::to_wstring(percent) + L"%"});
    } else if (phase == "cancelled" || phase == "idle") {
      // a canceled download lands on "cancelled" (rivet's managed-updater
      // vocabulary); restore the resting row, candidate stays retryable
      update_install_armed_ = false;
      StopUpdateTimer();
      set_cancel(false);
      set_status(hstring(tr_hstring("update.idle")));
      set_button(hstring(tr_hstring("update.check")), true);
    }
  } catch (std::exception const& e) {
    ShowError(winrt::to_hstring(e.what()));
    StopUpdateTimer();
  }
}

// ---------- install -------------------------------------------------------

void MainWindow::InstallVerifiedUpdate() {
  auto const backend = backend_;
  if (backend == nullptr || !backend->running()) return;
  try {
    rivet_app::API api(*backend);
    auto const artifact = api.verified_artifact().get();
    auto const path =
        std::filesystem::path{winrt::to_hstring(artifact).c_str()};
    if (!IsMsiInstall()) {
      LaunchPortableHandoff(path);
      return;
    }
    rivet::update::MsiHandoff::LaunchVerifiedInstaller(
        kUpdateApplicationId, path, executable_path().wstring());
  } catch (std::exception const& e) {
    ShowError(winrt::to_hstring(e.what()));
  }
}

// Portable install shape: write the swap batch, launch it detached, and
// exit. The batch waits for this PID, extracts the verified zip, swaps
// the install tree (.old fallback), relaunches, and leaves a marker —
// HandleInstallMarkers reports the outcome on the next launch.
void MainWindow::LaunchPortableHandoff(
    std::filesystem::path const& zip) {
  auto const work = update_work_dir();
  auto const script = work / L"update-install.cmd";
  std::error_code ec;
  std::filesystem::remove(update_failure_marker(), ec);
  std::filesystem::remove(update_success_marker(), ec);
  {
    std::ofstream file(script, std::ios::binary | std::ios::trunc);
    if (!file) {
      ShowError(tr_hstring("update.install-failed"));
      return;
    }
    auto const batch = portable_handoff_batch(::GetCurrentProcessId());
    file.write(batch.data(), static_cast<std::streamsize>(batch.size()));
    file.close();
    if (!file) {
      ShowError(tr_hstring("update.install-failed"));
      return;
    }
  }
  std::wstring const exe = executable_path().wstring();
  std::wstring const install_dir = executable_path().parent_path().wstring();
  std::wstring parameters =
      L"/c call \"" + script.wstring() + L"\" \"" + zip.wstring() +
      L"\" \"" + install_dir + L"\" \"" + exe + L"\" \"" +
      update_failure_marker().wstring() + L"\" \"" +
      update_success_marker().wstring() + L"\"";
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = L"cmd.exe";
  info.lpParameters = parameters.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) {
    ShowError(tr_hstring("update.install-failed"));
    return;
  }
  winrt::Microsoft::UI::Xaml::Application::Current().Exit();
}

// Surface a previous install attempt once at launch: a success marker
// means the swapped-in version launched — retire the .old fallback; a
// failure marker means the handoff failed — report the reason the batch
// left behind (MSI failures come through MsiHandoff::ConsumeFailure).
void MainWindow::HandleInstallMarkers() {
  std::error_code ec;
  auto const success = update_success_marker();
  if (!success.empty() && std::filesystem::exists(success, ec)) {
    std::filesystem::remove(success, ec);
    try {
      auto const install_dir = executable_path().parent_path();
      std::filesystem::remove_all(
          std::filesystem::path(install_dir.wstring() + L".old"), ec);
    } catch (...) {
      // The fallback copy is expendable; never block startup on it.
    }
    return;
  }
  // MSI shape: rivet keeps per-attempt failure markers under
  // %LOCALAPPDATA%\Rivet\updates\<id>\; ConsumeFailure reads the newest
  // and deletes it. Portable failures land in our own work dir.
  std::wstring reason;
  if (auto failure = rivet::update::MsiHandoff::ConsumeFailure(
          kUpdateApplicationId);
      failure.has_value()) {
    reason = failure->message;
  } else {
    auto const marker = update_failure_marker();
    if (!marker.empty() && std::filesystem::exists(marker, ec)) {
      std::ifstream file(marker);
      std::string code;
      std::getline(file, code);
      file.close();
      std::filesystem::remove(marker, ec);
      reason = winrt::to_hstring(code.empty() ? "update-install-failed"
                                              : code);
      return ShowInstallFailure(reason);
    }
  }
  if (!reason.empty()) {
    ShowInstallFailure(reason);
  }
}

void MainWindow::ShowInstallFailure(std::wstring const& reason) {
  ShowError(hstring{std::wstring{tr_hstring("update.install-failed")} +
                    L" " + reason});
}

// ---------- silent check --------------------------------------------------

// Taskly-family silent check (docs/UPDATE.md behavior contract): at most
// one network check per 4 hours, gated by the auto-check toggle and the
// dev-copy gate; the timestamp is stamped BEFORE the check so a failing
// check cannot hammer the feed, and check failures never surface to the
// user — only a newer version does (as an offer dialog). Manual checks
// from the settings dialog bypass the throttle and report every outcome.
winrt::fire_and_forget MainWindow::StartSilentUpdateCheck() {
  auto const backend = backend_;
  if (backend == nullptr || !backend->running()) co_return;
  if (IsDevCopy()) co_return;  // dev builds never phone home
  auto const weak = get_weak();
  auto const dispatcher = DispatcherQueue();
  try {
    co_await winrt::resume_background();
    rivet_app::API api(*backend);
    if (!api.get_auto_check_updates().get()) co_return;
    // UPDATE.md ordering rule: persist the install shape before asking
    // the backend to select an artifact (see RunUpdateFlow).
    (void)api.set_install_flavor(IsMsiInstall() ? "msi" : "portable").get();
    auto const now =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    auto const last = api.get_last_update_check().get();
    constexpr std::int64_t kSilentCheckInterval = 4 * 60 * 60;
    if (last > 0 && now - last < kSilentCheckInterval) co_return;
    (void)api.touch_last_update_check().get();
    (void)api.check_updates().get();
    if (api.updater_phase().get() != "available") co_return;
    auto const version = api.updater_available_version().get();
    if (version.empty()) co_return;
    dispatcher.TryEnqueue(
        [weak, version = winrt::to_hstring(version)] {
          if (auto window = weak.get()) {
            window->ShowUpdateOfferDialog(version);
          }
        });
  } catch (...) {
    // Silent mode: a failed check is not the user's problem.
  }
}

// The offer dialog walks offer → download → armed install on its primary
// button, keeping itself open the whole way (PrimaryButtonClick cancels
// the auto-close); its Cancel button closes the dialog and, while a
// download is in flight, cancels the transfer — the verified candidate
// stays on the backend so the offer can be retried.
void MainWindow::ShowUpdateOfferDialog(winrt::hstring const& version) {
  if (update_offer_dialog_ != nullptr || settings_dialog_ != nullptr) return;
  auto const weak = get_weak();
  auto dialog = ContentDialog();
  update_offer_dialog_ = dialog;

  dialog.Title(box_value(tr_hstring("update.available-title")));
  auto stack = StackPanel();
  stack.Spacing(8);
  auto versions = TextBlock();
  versions.Text(hstring{winrt::to_hstring(rivet_app::kVersion)} + L" → " +
                version);
  versions.FontSize(14);
  auto body = TextBlock();
  body.Text(tr_hstring("update.available-body"));
  body.TextWrapping(TextWrapping::Wrap);
  auto status = TextBlock();
  status.Opacity(0.75);
  status.FontSize(12);
  status.Visibility(Visibility::Collapsed);
  stack.Children().Append(versions);
  stack.Children().Append(body);
  stack.Children().Append(status);

  dialog.Content(stack);
  dialog.PrimaryButtonText(tr_hstring("update.install-now"));
  dialog.CloseButtonText(tr_hstring("settings.cancel"));
  dialog.DefaultButton(ContentDialogButton::Primary);
  dialog.XamlRoot(Content().XamlRoot());

  dialog.PrimaryButtonClick(
      [weak, dialog, status](auto const&,
                             ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);
        auto const window = weak.get();
        if (!window) return;
        auto const backend = window->backend_;
        if (!backend || !backend->running()) return;
        if (window->update_offer_armed_) {
          window->InstallVerifiedUpdate();  // the handoff terminates the app
          return;
        }
        if (window->update_offer_downloading_) return;
        try {
          rivet_app::API api(*backend);
          (void)api.start_download().get();
        } catch (...) {
          return;
        }
        window->update_offer_downloading_ = true;
        dialog.IsPrimaryButtonEnabled(false);
        status.Visibility(Visibility::Visible);
        status.Text(tr_hstring("update.downloading"));
        window->update_offer_timer_ = DispatcherTimer();
        window->update_offer_timer_.Interval(
            std::chrono::milliseconds{1000});
        window->update_offer_timer_.Tick(
            [weak, dialog, status](auto const&, auto const&) {
              auto const window = weak.get();
              if (!window) return;
              auto const backend = window->backend_;
              if (!backend || !backend->running()) {
                window->update_offer_timer_.Stop();
                return;
              }
              try {
                rivet_app::API api(*backend);
                auto const phase = api.updater_phase().get();
                if (phase == "downloaded") {
                  window->update_offer_timer_.Stop();
                  window->update_offer_downloading_ = false;
                  window->update_offer_armed_ = true;
                  dialog.IsPrimaryButtonEnabled(true);
                  dialog.PrimaryButtonText(tr_hstring("update.install"));
                  status.Text(tr_hstring("update.downloaded"));
                } else if (phase == "downloading" || phase == "available") {
                  status.Text(
                      hstring{std::wstring{tr_hstring("update.progress")} +
                              L" " +
                              std::to_wstring(api.updater_percent().get()) +
                              L"%"});
                } else if (phase == "error") {
                  window->update_offer_timer_.Stop();
                  window->update_offer_downloading_ = false;
                  dialog.IsPrimaryButtonEnabled(true);
                  dialog.PrimaryButtonText(tr_hstring("update.install-now"));
                  auto const message = api.updater_message().get();
                  status.Text(winrt::to_hstring(
                      message.empty() ? "update failed" : message));
                } else if (phase == "cancelled" || phase == "idle") {
                  // canceled — back to a clean offer for a retry
                  window->update_offer_timer_.Stop();
                  window->update_offer_downloading_ = false;
                  dialog.IsPrimaryButtonEnabled(true);
                  dialog.PrimaryButtonText(tr_hstring("update.install-now"));
                  status.Visibility(Visibility::Collapsed);
                }
              } catch (...) {
                window->update_offer_timer_.Stop();
                window->update_offer_downloading_ = false;
                dialog.IsPrimaryButtonEnabled(true);
                dialog.PrimaryButtonText(tr_hstring("update.install-now"));
              }
            });
        window->update_offer_timer_.Start();
      });

  auto const on_closed = [weak](auto const&, auto const&) {
    auto const window = weak.get();
    if (!window) return;
    window->update_offer_timer_.Stop();
    window->update_offer_dialog_ = nullptr;
    if (window->update_offer_downloading_) {
      window->update_offer_downloading_ = false;
      auto const backend = window->backend_;
      if (backend && backend->running()) {
        try {
          rivet_app::API api(*backend);
          (void)api.cancel_download().get();
        } catch (...) {
        }
      }
    }
  };
  dialog.Closed(on_closed);
  dialog.ShowAsync().Completed(on_closed);
}

void MainWindow::OpenReleasesPage() {
  try {
    auto const page = winrt::Windows::Foundation::Uri{kReleasesUrl};
    (void)Windows::System::Launcher::LaunchUriAsync(page);
  } catch (...) {
  }
}

}  // namespace winrt::RivetHost::implementation
