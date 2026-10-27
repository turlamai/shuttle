#pragma once

// Main-thread bridge for GTK hosts (issue #247): backend completions and
// events arrive on the embedded runtime's reader thread, and nothing in
// that context may touch GTK objects. Hosts flatten one completion into an
// Unpacked<T> (value or error, no exception flow across threads) and hand a
// closure to post_to_main, which runs it on the GTK main loop and frees it
// there. The template main.cpp shows the pattern end to end.

#include <glib.h>

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "GeneratedBackend.hpp"

namespace rivet_app {

// One completion, flattened for widget code: exactly one of value/error is
// meaningful, decided on the reader thread before the hop.
template <typename T>
struct Unpacked {
  bool ok{false};
  T value{};
  std::string error;
};

template <>
struct Unpacked<void> {
  bool ok{false};
  std::string error;
};

template <typename T>
Unpacked<T> unpack(Result<T> result) {
  Unpacked<T> flat;
  try {
    if constexpr (!std::is_void_v<T>) {
      flat.value = result.get();
    } else {
      result.get();
    }
    flat.ok = true;
  } catch (std::exception const& error) {
    flat.error = error.what();
  } catch (...) {
    flat.error = "unknown backend failure";
  }
  return flat;
}

// Runs work on the GTK main loop. The closure is heap-allocated on the
// calling (reader) thread and freed on the main thread no matter which
// GSource callback result the invocation produces.
inline void post_to_main(std::function<void()> work) {
  auto* pending = new std::function<void()>(std::move(work));
  g_idle_add(
      +[](gpointer user_data) -> gint {
        std::unique_ptr<std::function<void()>> work(
            static_cast<std::function<void()>*>(user_data));
        (*work)();
        return G_SOURCE_REMOVE;
      },
      pending);
}

}  // namespace rivet_app
