// Rivet Linux host scaffold — GTK4 window over one embedded Racket CS
// backend. Mirrors the WinUI scaffold: boot the runtime off the UI thread,
// render the counter State through the generated client, dispatch every
// completion back to the main loop before touching widgets.
#include <gtk/gtk.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "GeneratedBackend.hpp"
#include "layout.hpp"
#include "rpc_bridge.hpp"
#include "system_services.hpp"
#include "theme.hpp"

namespace {

struct AppState {
  GtkWindow* window{nullptr};
  GtkLabel* status{nullptr};
  GtkLabel* count_label{nullptr};
  GtkButton* increment{nullptr};

  std::unique_ptr<rivet::linux_runtime::Backend> backend;
  std::unique_ptr<rivet_app::API> api;
  std::mutex startup_mutex;
  std::thread startup_thread;
  std::unique_ptr<rivet::linux_runtime::Backend> startup_backend;
  std::string startup_error;
  std::atomic<bool> shutting_down{false};
  std::int64_t count_value{0};

  void set_status(std::string const& text) {
    gtk_label_set_text(status, text.c_str());
  }

  void set_count(std::int64_t value) {
    count_value = value;
    gtk_label_set_text(count_label,
                       ("Count: " + std::to_string(value)).c_str());
  }
};

AppState g_state;

void perform_orderly_shutdown();

// One completion, flattened once, dispatched once: unpack() runs on the
// backend reader thread, post_to_main moves the flattened result to the
// GTK main loop, and the closure frees itself there (issue #247).
void deliver_count_result(rivet_app::Result<std::int64_t> result) {
  rivet_app::post_to_main(
      [flat = rivet_app::unpack(std::move(result))]() mutable {
        if (flat.ok) {
          g_state.set_count(flat.value);
        } else {
          g_state.set_status("State error: " + flat.error);
        }
        gtk_widget_set_sensitive(GTK_WIDGET(g_state.increment), TRUE);
      });
}

int on_backend_finished(gpointer) {
  if (g_state.startup_thread.joinable()) {
    g_state.startup_thread.join();
  }

  std::unique_ptr<rivet::linux_runtime::Backend> backend;
  std::string error;
  {
    std::lock_guard lock(g_state.startup_mutex);
    backend = std::move(g_state.startup_backend);
    error = std::move(g_state.startup_error);
  }

  if (g_state.shutting_down.load(std::memory_order_acquire)) {
    if (backend != nullptr) {
      backend->stop();
    }
    return G_SOURCE_REMOVE;
  }

  if (!error.empty()) {
    g_state.set_status("Backend error: " + error);
    return G_SOURCE_REMOVE;
  }
  if (backend == nullptr) {
    g_state.set_status("Backend error: startup completed without a backend");
    return G_SOURCE_REMOVE;
  }

  g_state.backend = std::move(backend);
  g_state.api = std::make_unique<rivet_app::API>(*g_state.backend);

  // racket_boot installs Racket CS signal handlers. Install Rivet's process
  // shutdown bridge after the Hello handshake so SIGTERM/SIGINT keep reaching
  // the native application's orderly close path.
  try {
    rivet::system::InstallShutdownHook(perform_orderly_shutdown);
  } catch (std::exception const& hook_error) {
    std::fputs((std::string{"shutdown hook unavailable: "} +
                hook_error.what() + "\n")
                   .c_str(),
               stderr);
  }

  g_state.set_status("Embedded Racket CS is ready");
  gtk_widget_set_sensitive(GTK_WIDGET(g_state.increment), TRUE);
  (void)g_state.api->get_counter_async(deliver_count_result);
  return G_SOURCE_REMOVE;
}

void start_backend() {
  // The runtime library owns the layout contract (issue #246); hosts locate
  // stage roots through it instead of re-deriving the paths.
  auto layout = rivet::linux_runtime::discover_layout();
  if (!layout.has_value()) {
    g_state.set_status(rivet::linux_runtime::layout_error_hint());
    return;
  }

  rivet::linux_runtime::RacketRuntimeConfig config;
  config.executable_path =
      rivet::linux_runtime::executable_path().string();
  config.petite_boot = layout->petite_boot.string();
  config.scheme_boot = layout->scheme_boot.string();
  config.racket_boot = layout->racket_boot.string();
  config.backend_bundle = layout->core.string();
  config.module_name = rivet_app::kModuleName;
  config.entry_symbol = rivet_app::kEntryName;

  // Booting the embedded runtime blocks on file I/O; only startup runs off
  // the main loop. Everything after completion dispatches back through
  // g_idle_add.
  g_state.startup_thread = std::thread([config = std::move(config)]() mutable {
    auto backend =
        std::make_unique<rivet::linux_runtime::Backend>(std::move(config));
    try {
      backend->start();
      {
        std::lock_guard lock(g_state.startup_mutex);
        g_state.startup_backend = std::move(backend);
      }
    } catch (std::exception const& e) {
      std::lock_guard lock(g_state.startup_mutex);
      g_state.startup_error = e.what();
    }
    g_idle_add(on_backend_finished, nullptr);
  });
}

void on_increment_clicked(GtkButton*, gpointer) {
  if (g_state.api == nullptr) {
    return;
  }
  gtk_widget_set_sensitive(GTK_WIDGET(g_state.increment), FALSE);
  auto const next = g_state.count_value + 1;
  (void)g_state.api->set_counter_async(next, deliver_count_result);
}

void on_activate(GtkApplication* app, gpointer) {
  if (g_state.window != nullptr) {
    gtk_window_present(g_state.window);
    return;
  }

  // Plain GTK4 can receive a portal color scheme that disagrees with the
  // selected theme-name variant. Pin both before creating widgets so native
  // controls and application CSS cannot render on opposite palettes.
  (void)rivet::linux_ui::ApplyTheme();

  auto* window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "Rivet — Racket + GTK4");
  gtk_window_set_default_size(GTK_WINDOW(window), 460, 320);

  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_top(root, 40);
  gtk_widget_set_margin_bottom(root, 40);
  gtk_widget_set_valign(root, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(root, GTK_ALIGN_CENTER);

  auto* title = gtk_label_new("Rivet");
  gtk_widget_add_css_class(title, "title-1");
  auto* subtitle = gtk_label_new("Racket + GTK4");
  gtk_widget_add_css_class(subtitle, "dim-label");
  auto* status = gtk_label_new("Starting embedded Racket CS…");
  gtk_widget_add_css_class(status, "dim-label");
  auto* count = gtk_label_new("Count: 0");
  gtk_widget_add_css_class(count, "title-2");
  auto* increment = gtk_button_new_with_label("Increment in Racket");
  gtk_widget_set_sensitive(increment, FALSE);
  gtk_widget_set_hexpand(increment, TRUE);

  gtk_box_append(GTK_BOX(root), title);
  gtk_box_append(GTK_BOX(root), subtitle);
  gtk_box_append(GTK_BOX(root), status);
  gtk_box_append(GTK_BOX(root), count);
  gtk_box_append(GTK_BOX(root), increment);
  gtk_window_set_child(GTK_WINDOW(window), root);

  g_state.status = GTK_LABEL(status);
  g_state.count_label = GTK_LABEL(count);
  g_state.increment = GTK_BUTTON(increment);
  g_state.window = GTK_WINDOW(window);
  g_signal_connect(increment, "clicked", G_CALLBACK(on_increment_clicked),
                   nullptr);

  gtk_window_present(GTK_WINDOW(window));
  start_backend();
}

void on_shutdown(GApplication*, gpointer) {
  g_state.shutting_down.store(true, std::memory_order_release);
  if (g_state.startup_thread.joinable()) {
    g_state.startup_thread.join();
  }

  std::unique_ptr<rivet::linux_runtime::Backend> startup_backend;
  {
    std::lock_guard lock(g_state.startup_mutex);
    startup_backend = std::move(g_state.startup_backend);
  }
  if (startup_backend != nullptr) {
    startup_backend->stop();
  }
  if (g_state.backend != nullptr) {
    g_state.backend->stop();
  }
}

// SIGTERM/SIGINT bypass the GTK main loop, so the shutdown signal handler
// alone would leave the window alive over a dead backend. The watcher
// thread runs this same cleanup and then hard-exits; keep it synchronous
// and allocation-light.
void perform_orderly_shutdown() { on_shutdown(nullptr, nullptr); }

}  // namespace

int main(int argc, char** argv) {
  g_set_application_name("Rivet");
  auto* app = gtk_application_new("dev.rivet.host",
                                  G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), nullptr);
  int const status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
