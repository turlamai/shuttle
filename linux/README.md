# Linux host (GTK4)

The third first-party host: a GTK4 window over one embedded Racket CS
backend, speaking RVT1 through the shared `runtime/` codec. The embedding
contract mirrors `platform/windows/runtime` — the same public Racket CS C API
and the same threading rules — with a connected `socketpair` standing in for
the Win32 named pipe pair. The Racket input and output ports receive distinct
descriptors for that socket so their ownership and shutdown behavior are
unambiguous.

Status: **developer preview**. CI compiles the runtime bridge against Racket's
public embedding headers and exercises startup, concurrent RPCs, State access,
cancellation, overload, shutdown, project scaffolding, CLI build, packaging,
verification, and release against a real embedded Racket CS instance.

## Layout

```text
platform/linux/
├── runtime/
│   ├── backend.hpp        # rivet::linux_runtime::Backend — same contract as rivet::windows
│   └── backend.cpp        # racketcs boot + RVT1-over-socketpair transport
├── system/
│   ├── system_services.hpp  # public rivet::system surface
│   ├── system_services.cpp  # capability discovery + activation arguments
│   ├── clipboard.cpp        # GTK4/GDK text clipboard + change observation
│   ├── file_dialogs.cpp     # GTK4 native/portal open and save dialogs
│   ├── single_instance.cpp  # local activation lease and forwarding
│   ├── notifications.cpp    # freedesktop notifications
│   ├── global_shortcuts.cpp # XDG GlobalShortcuts portal
│   ├── tray.cpp             # StatusNotifierItem + dbusmenu
│   ├── autostart.cpp        # XDG autostart entries
│   ├── secure_storage.cpp   # Secret Service through libsecret
│   └── crash_shutdown.cpp   # fatal-signal record + orderly termination
└── host/
    ├── GeneratedBackend.hpp  # scaffold schema; raco rivet build replaces it
    ├── CMakeLists.txt
    └── src/
        ├── main.cpp          # GTK4 counter demo over the generated client
        └── rpc_bridge.hpp    # completion → main-loop marshaling (below)
```

## Building by hand

Requirements: an embeddable Racket CS build, CMake ≥ 3.24, pkg-config, GTK 4,
zlib, LZ4, curses, and a graphical session (or headless Weston) to run. The standard
prebuilt Linux Racket installer does not ship `libracketcs` or the three boot
files; build and install
Racket CS from a source distribution as described by Racket's embedding guide.
The CI workflow uses the official minimal "source + built libraries" archive so
the build remains reasonably small.

```bash
export RIVET_ROOT="$PWD"
export RIVET_RACKET_INCLUDE=/path/to/racket/include
export RIVET_RACKET_LIBRARY=/path/to/racket/lib/libracketcs.a
cmake -S platform/linux/host -B /tmp/rivet-linux-build
cmake --build /tmp/rivet-linux-build
```

The executable must sit beside a staged runtime to start: put `runtime/*.boot`
and `res/core.zo` (produced by `raco ctool --mods`, the same artifacts every
platform host consumes) next to `RivetHost`. `raco rivet build`, `dev`, and
`package` create this layout automatically.

### Runtime layout discovery

Do not re-derive the layout in application code. The platform library owns
the contract (`rivet::linux_runtime`, `layout.hpp`):

```cpp
auto layout = rivet::linux_runtime::discover_layout();
if (!layout.has_value()) {
  show(rivet::linux_runtime::layout_error_hint());  // names the missing files
}
// layout->petite_boot / scheme_boot / racket_boot / core / stage_root
```

`discover_layout()` looks beside the running executable; an explicit-root
overload exists for tests and unusual staging. `raco rivet build`, `dev`,
and `package` produce exactly the tree it looks for.

### Completions belong on the main loop

Backend completions and events arrive on the runtime's reader thread, and
nothing in that context may touch GTK objects. Use the shipped bridge
(`src/rpc_bridge.hpp`) instead of hand-rolling `g_idle_add` plumbing:

```cpp
#include "rpc_bridge.hpp"

void deliver_count(rivet_app::Result<std::int64_t> result) {
  rivet_app::post_to_main(
      [flat = rivet_app::unpack(std::move(result))]() mutable {
        if (flat.ok) { set_count(flat.value); }
        else { set_status("State error: " + flat.error); }
      });
}
```

`unpack` flattens `Result<T>` into `Unpacked<T>` (value or error, decided on
the reader thread — no exception flow crosses the hop); `post_to_main` runs
the closure on the GTK main loop and frees it there. `main.cpp` shows the
pattern end to end.

## Display policy

Wayland is the primary Linux target. CI starts a headless Weston compositor,
unsets `DISPLAY`, selects `GDK_BACKEND=wayland`, and requires the packaged host
to survive the launch smoke. Rivet does not force that environment variable in
shipped applications: GTK may still select a distribution-provided X11 backend
for compatibility. X11 and XWayland are best-effort paths, not release gates,
and Rivet does not add X11-only application APIs. See
[Linux display policy](../../../docs/linux-display.md).

## Honest gaps

- Production releases use `raco rivet release`: a deterministic self-contained
  `.tar.gz` with a detached Ed25519 signature, plus deb/rpm/AppImage formats and
  a signed portable-zip update feed. Real install/upgrade evidence across the
  supported distribution matrix remains follow-up work.
- The system adapter covers single-instance, XDG portal global shortcuts,
  notifications, StatusNotifierItem tray integration, XDG autostart, Secret
  Service secure storage, crash hooks, graceful shutdown, and runtime
  capability reporting. Global shortcuts remain compositor-controlled and
  report unavailable instead of falling back to X11-only `XGrabKey`.
- GTK is a toolkit, not a display protocol: always-on-top overlays and other
  compositor-specific features still require a capability check and an
  appropriate Wayland protocol or portal.
