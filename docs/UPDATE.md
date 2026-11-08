# Shuttle Update Contract

> Online updates are the release vehicle: every installed Shuttle copy
> can check, download, verify and install a new version without hunting
> for download pages. The behavior follows the family contract (taskly's
> `shared/spec/UPDATE.md`); the transport is rivet's Ed25519-signed
> manifest feed, the install mechanism is native to each install shape.

## Behavior contract

- **Triggers**: one silent check shortly after launch (throttled: at
  most once per 4 hours, persisted as config key `last-update-check`,
  unix epoch seconds, backend-owned via `get-last-update-check` /
  `touch-last-update-check`), plus a manual `检查更新` button in
  Settings that bypasses the throttle. The throttle timestamp is
  stamped BEFORE the check so a failing check cannot hammer the feed.
- **Auto-check toggle**: config key `auto-check-updates` (default on),
  exposed as the 自动检查更新 switch in Settings. When off, the silent
  launch check skips; the manual button always checks and is unaffected
  by the toggle or the throttle.
- **Dev copies never phone home**: a copy without the packaged layout
  (`res/` + `runtime/` beside the exe), or one running from a temp
  path, is a dev build — silent checks skip and the manual button says
  so (`update.dev-copy`).
- **Silent mode**: network failure / rate limit → silently ignore
  (never nag). Manual mode reports every outcome.
- **Newer version found** (semver compare, downgrades ignored) → offer
  dialog with the new version: `立即更新 / Update now` (download →
  install, one button walks the whole path with live percent) or
  `取消 / Cancel`. Up-to-date → `已是最新版本` in the settings row.
- **Download**: performed by the backend through rivet's
  managed-updater over the `check-updates` / `start-download` /
  `cancel-download` / `updater-phase` RPCs (Ed25519 manifest verify and
  sha256 artifact verify live in the backend; hosts never implement
  crypto). Hosts poll `updater-phase` (phase: idle | checking |
  available | up-to-date | downloading | downloaded | cancelled |
  error) and show progress.
- **Download must be bounded and cancellable**: a stalled transfer is
  broken by rivet's watchdog (no byte progress for the stall limit, or
  wall clock past the total limit) and surfaces a retryable error; the
  Cancel button interrupts the worker, removes partial data, and
  preserves the verified candidate so the offer can be retried.
- **Install failure** → non-crashing error with the platform reason;
  the running version keeps working.
- **Install shape ordering rule**: the host persists the detected
  install flavor (`set-install-flavor`) BEFORE every check, so an MSI
  install is never offered the zip and vice versa.

## Platform mechanisms

The release pipeline signs and uploads `update-stable.json` (the
family signed-wrapper format) for every release, plus versioned and
versionless (stable-alias) MSI + zip assets.

| Install shape | Feed artifact | Install | Integrity |
|---|---|---|---|
| Windows MSI | signed `windows`/`x64` `msi` entry | `MsiHandoff`: a detached batch waits for app exit → `msiexec /passive /norestart /i` (per-machine installs surface one UAC consent) → relaunch | Ed25519 wrapper signature + signed size/SHA-256 |
| Windows portable zip | signed `windows`/`x64` `zip` entry | handoff `update-install.cmd` in `%TEMP%\shuttle-update`: wait for app exit (≤30 s) → `tar -xf` extract → swap install dir in place (`.old` copy kept) → relaunch; failure marker (`update-failed.txt`) reported on next launch | Ed25519 wrapper signature + signed size/SHA-256 |
| Dev checkout | — | updates refuse (`updates require an installed Shuttle copy`); the UI gates earlier via the dev-copy check | — |

The final handoff dispatches from the host-detected flavor and the
verified download's extension, never a second install-location guess.

## Installed lifecycle, backup and recovery

Application files and user data are separate. User data lives under
`%APPDATA%\shuttle\` (config.json, history.jsonl, the updates download
staging) — every upgrade path replaces only the application shape and
must not touch it. The release evidence suite proves this boundary.

| Install shape | Normal N→N+1 path | Failure / rollback | Manual recovery |
|---|---|---|---|
| Windows MSI | verified MSI runs passive msiexec; installed-product upgrade rules replace program files | nonzero exit / timeout writes a failure marker; the old running process stays usable | rerun the previous trusted MSI Repair/Install; never delete the data directory |
| Windows portable zip | verified zip extracts, old dir renames to `<dir>.old`, new tree moves in, app relaunches | the batch restores the renamed directory and records a failure marker (`extract-failed` / `rename-failed` / `swap-failed` / `process-exit-timeout`) | quit all copies, restore the `.old` directory or extract the previous trusted zip to a new program directory |

Next-launch marker reporting: a success marker retires the `.old`
fallback copy; a failure marker (MSI via
`MsiHandoff::ConsumeFailure`, portable via the work-dir marker) is
read once, deleted, and surfaced through the error bar with the batch
reason — the running version keeps working either way.

## `update-stable.json`

Generated by `raco rivet release` from the release artifacts and
uploaded as a single release asset. Inner manifest (decoded
base64 `payload`) — rivet's manifest schema with
`application_id = "dev.rivet.shuttle"` (rivet.rktd's `identifier`;
the release preflight pins the backend constant to it). Clients fetch
`https://github.com/turlamai/shuttle/releases/latest/download/update-stable.json`
(the moving "latest" location; redirect-following is mandatory).
Artifact URLs inside the manifest are pinned to the concrete tag, and
both the signing and parsing layers refuse non-HTTPS URLs.

- `version` must equal the tag, `rivet.rktd`, the generated client
  `kVersion`, and the backend `app-version` (release preflight checks
  all, plus the updater identity pair).
- A sticky per-install rollout bucket (config key `rollout-bucket`,
  integer 0–99) gates staged rollouts against the manifest's
  `rollout` percentage.

## Release evidence (acceptance kind `release`)

The update chain carries its own acceptance requirement
(`release-update` suite in `rivet-acceptance.rktd`,
`tests/release-update-test.rkt`), and the release pipeline runs it on
the exact tree being published — a release cannot ship without its
update evidence. The suite proves, at the backend boundary every host
shares:

- the manifest transport gate is HTTPS-only at both the updater
  constructor and the low-level fetch (a plain-http feed can never
  yield a candidate);
- a feed signed by another key, or under another key-id, fails
  verification (the embedded production key is the only trust anchor);
- semver refuses downgrades and `rollout: 0` gates the offer;
- the N→N+1 lifecycle downloads through the real managed-updater,
  verifies the artifact SHA-256 against the signed manifest, renames
  the `.partial` into place, and leaves user data byte-identical;
- a sha-mismatched artifact never reaches the destination and the
  worker returns to a retryable state;
- the install flavor selects the matching signed artifact (msi for
  MSI installs, zip for portable copies) from the same manifest.

Cancel, stall-watchdog and timeout behavior live inside rivet's
managed-updater and are covered by rivet's own suite. The mailbox
protocol chain (bridge kind) is exercised hermetically by
`app/backend.rkt`'s `test-main`.

## Network transparency

The update feed is the only network use beyond the mailbox share:
Shuttle ships no telemetry, accounts, or other connections. Denying
the feed leaves every other feature fully functional offline.

## Ed25519 signature

- Key type: Ed25519. Private key lives ONLY in the GitHub secret
  `UPDATE_ED25519_PRIVATE_KEY` (DER) and the release manager's local
  backup — never in the repository.
- Public key: embedded in `app/backend.rkt` as SPKI DER base64 (the
  format `bytes->ed25519-public-key` parses). `key_id` is
  `update-2026-10`; clients pin it. Rotate = ship a build that trusts
  the next key before releases stop being signed with the old one.
- Verification: wrapper → schema 1 → key id → Ed25519 over the exact
  payload bytes; then size + sha256 of the downloaded artifact against
  the signed manifest. Any mismatch aborts the update (silently in
  silent mode, with an error in manual mode).
- Error reporting: every error the UI shows carries a 复制错误 button
  putting the verbatim message on the clipboard — a bug report starts
  from that string.
