# Shuttle agent contract

Shuttle is a **Windows-only** Rivet application: two company PCs exchange
files through a mailbox folder on the department share (`Z:`). The macOS and
Linux host directories were intentionally removed; do not recreate them.

## Start every task here

```bash
raco rivet inspect --json
raco rivet doctor --json
```

`inspect` is the machine-readable project map. `doctor` reports the local
toolchain (MSVC Build Tools + Windows SDK are required for the WinUI 3 host)
and returns a failing exit status when the current host is not usable.

Before editing, check `git status`. If the worktree is dirty, the branch has
diverged, or the remote is unavailable, stop and report that state; never hide
it with stash, reset, or generated-file cleanup.

## Architecture boundary

- All product logic (mailbox protocol, icacls self-heal, polling, config,
  history) lives in `app/backend.rkt`. The backend is the single source of
  truth; native hosts only present and forward OS integration.
- Windows UI is native WinUI 3 / C++/WinRT under `windows/`: presentation,
  drag-drop, tray, toasts, autostart registry, pickers.
- Do not introduce a cross-platform UI DSL or bypass RVT1 with an ad-hoc IPC.
- Relay mailbox protocol invariants (do not break without bumping the
  protocol): manifest.json is written FIRST for crash attribution, payload is
  copied as `payload.part` then renamed, `shuttle.done` is written LAST;
  receivers only act on folders with `.done`; only folders whose manifest
  names the local machine as sender are ever deleted by the sweep.

## Share permission model (do not "fix" these away)

- The department share gives Domain Users create-but-not-delete on public
  folders. Shuttle's countermeasure is self-healing: right after creating a
  transfer folder, the backend grants itself `(OI)(CI)F` via icacls (never
  `/t` — server-side inheritance covers the whole subtree).
- Deletion on the share is permanent (no recycle bin). Receivers delete the
  remote folder only after the local copy is fully written and size-verified.
- Test against a dedicated `.shuttle-selftest` mailbox, never the real one.

## Ownership

Source you may edit:

- `app/`
- `windows/`
- `shared/i18n/`
- `rivet.rktd`
- `rivet-schema.json` when an API compatibility break is intentional
- `rivet-acceptance.rktd` when product acceptance requirements change
- declared application resources

Generated output you should not hand-edit:

- `windows/GeneratedBackend.hpp` (kept compiling between builds; the real
  build regenerates it)
- `windows/GeneratedI18n.hpp`
- `.rivet/`, `build/`, `dist/`
- `windows/Generated Files/`, `windows/obj/`, `windows/RivetHost/`

`raco rivet i18n` also rewrites `macos-host/GeneratedI18n.swift` even though
the directory does not exist for this product; that path is gitignored.

## Verification loop

This project includes a source-controlled `rivet-schema.json` API baseline.
Refresh it only after deliberately changing the public backend schema:

```bash
raco rivet schema --output rivet-schema.json
```

Compatibility gate before accepting later API changes (adding types is
compatible; removing or reordering is breaking):

```bash
raco rivet schema check rivet-schema.json --json
```

Backend engine tests (pure units + full mailbox chain against the real
share, peer identity simulated):

```bash
raco test app/backend.rkt
racket -e '(require (submod "app/backend.rkt" test)) (test-main)'
```

`rivet-acceptance.rktd` is the source-controlled completion contract. Add a
suite for every behavior a change claims, and never weaken requirements merely
to make a change green. Run the required suites with:

```bash
raco rivet acceptance --json
```

```bash
raco rivet build    # compile backend, regenerate clients, build WinUI host
raco rivet dev      # build + launch
raco rivet package  # distributable
```

After changing RPC, Event, State, Record, or Enum declarations, run the
compatibility gate and rebuild before editing code that consumes generated
native APIs. Preserve the first `rivet: error:` line and nearby compiler
output when diagnosing a failure.
