# Contributing to Shuttle

Thanks for helping improve Shuttle. This guide covers the development
setup and the house rules that keep the project at production standard.

## Development environment

Building the WinUI 3 host needs three things; CI has all of them, local
machines need them once:

1. **Racket CS 9.x** (with `raco`)
2. **The rivet framework**: `raco pkg install --auto rivet`
3. **MSVC Build Tools** with the C++ workload **plus the UWP C++
   component** — the WinUI 3 XAML compile pipeline comes from the
   `Application Type\Windows Store` targets that only the UWP component
   installs. VCTools alone builds plain C++ but fails with
   `MSB8020`/`MSB8036` on this project:

   ```bat
   winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Workload.UniversalBuildTools --includeRecommended"
   ```

Verify with `raco rivet doctor` — it should report the toolchain usable.

## Development loop

```bash
raco rivet dev        # build + launch the app
raco test app/backend.rkt          # backend unit tests (no share needed)
racket -e '(require (submod "app/backend.rkt" test)) (test-main)'   # full mailbox selftest on the real share
raco rivet schema check rivet-schema.json --json   # API compatibility gate
```

The full selftest writes a `.shuttle-selftest` mailbox **next to your
configured relay directory** (never the real one), sends under a fake
machine identity, receives as itself, and cleans up. It needs the
mapped share; everything else runs anywhere.

## House rules

- **Layers**: product logic lives in `app/backend.rkt`; `windows/` is
  presentation and OS integration only. If a feature can be expressed
  as a pure function over paths and JSON, it belongs in the backend.
- **Schema gate**: after changing any RPC/Event/Record declaration, run
  `raco rivet schema --output rivet-schema.json` and commit the
  refreshed baseline together with the code. Breaking RPC changes are
  fine (host and backend ship together) but must be deliberate.
- **Rivet is a pinned dependency**: bump `RIVET_PIN` in both workflows
  deliberately (full SHA from rivet main). Upstream problems go to the
  rivet issue tracker.
- **main stays green**: CI runs the schema gate, the full native build,
  and backend unit tests on every push.
- **Version contract**: `rivet.rktd`, `app/backend.rkt`
  (`app-version`), and the generated client (`kVersion`) must agree;
  `scripts/check-release-version.sh` verifies the triple on release
  tags (annotated tags only).

## Releases

Releases are tag-driven and fully automated: push an annotated
`vX.Y.Z` tag and CI builds the signed MSI + portable zip, the SHA-256
sidecars, and the signed update manifest. The in-app updater on MSI
installs picks it up automatically. See
[PROTOCOL.md](PROTOCOL.md) for the mailbox wire protocol and
[ARCHITECTURE.md](ARCHITECTURE.md) for the layer boundaries before
proposing structural changes.
