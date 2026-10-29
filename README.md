# Shuttle

[English](README.md) · [简体中文](README.zh-CN.md)

[![release](https://img.shields.io/github/v/release/turlamai/shuttle)](https://github.com/turlamai/shuttle/releases/latest) ![platform](https://img.shields.io/badge/platform-Windows-lightgrey) ![built with](https://img.shields.io/badge/built%20with-Rivet-9333ea) [![CI](https://github.com/turlamai/shuttle/actions/workflows/ci.yml/badge.svg)](https://github.com/turlamai/shuttle/actions/workflows/ci.yml)

"Direct" file transfer between two company Windows PCs. Drop a file on
machine A, machine B receives it automatically with a Windows toast — it
looks like a peer-to-peer transfer, but the bytes travel through a mailbox
folder on the department file share.

The problem it solves: a ZIP created on an EFS-encrypted machine cannot be
decrypted when sent to another PC directly, yet copying it through the
network share works fine. Shuttle automates that manual detour into one
drag-and-drop.

## Usage

1. **Send**: drop files onto the Shuttle window (or click the drop zone to
   pick). Files land in the mailbox folder on the share.
2. **Receive**: the other PC's Shuttle polls the mailbox every ~1.5 s,
   pulls new files into `Downloads\Shuttle`, fires a Windows notification,
   and cleans up the mailbox residue.
3. Closing the window minimizes to the tray — receiving stays online. A
   real exit lives in the tray menu. Sign-in autostart is a setting.

## One-time setup on each PC

1. Build or obtain the Shuttle folder on the first PC (see below), copy it
   to the second PC (`raco rivet package` output works too).
2. Start Shuttle on both machines and open ⚙ Settings:
   - **Relay directory**: must be **the same path on both PCs** (default
     `Z:\19 汽车电子事业部\研发部\04_Software\LiDAR\.shuttle`; any writable
     share location works);
   - **Download directory**: where received files land (default
     `Downloads\Shuttle`).
3. Tick sign-in autostart if wanted (HKCU Run key, no admin needed).

Both PCs sign in with the same domain account; transfers are disambiguated
by machine name, so the two sides never fight over a transfer.

### Multi-user deployments (colleagues)

Shuttle is a **pair** tool: the mailbox has no recipient addressing, and a
receiver claims any transfer not sent by itself. Multiple pairs must
therefore use **separate relay directories** — one per pair of machines:

```
Z:\...\04_Software\LiDAR\.shuttle              # your pair
Z:\...\04_Software\LiDAR\.shuttle-zhangsan     # colleague's pair
Z:\...\04_Software\LiDAR\.shuttle-lisi         # another colleague
```

Each person sets their own relay path in Settings on both of their
machines; everything else (notifications, cleanup, permissions
self-heal) is pair-local and needs no further setup. Do **not** point two
pairs at the same directory — the first idle machine will collect the
other pair's mail. (Colleagues from other departments: make sure the
chosen directory grants your account write access, and note that files
sit in the mailbox in transit where anyone with share access can read
them — the same trust level as the share itself.)

## Distribution (GitHub Releases, taskly family conventions)

Releases are fully CI-built — **no local MSVC toolchain needed** (the
runner has one):

1. Bump the version in `rivet.rktd`, add a `## [X.Y.Z]` section to
   `CHANGELOG.md`, commit and push.
2. Push an annotated tag; CI does the rest:

   ```bash
   git tag -a v0.1.0 -m "Shuttle 0.1.0" && git push origin v0.1.0
   ```

3. Release assets follow the family naming convention:
   - `shuttle-X.Y.Z-windows-x64.msi` / `.zip` (portable), each with a
     `.sha256` sidecar, plus an aggregated `SHA256SUMS`;
   - versionless stable aliases `shuttle-windows-x64.msi` / `.zip` with
     permanent URLs at `releases/latest/download/<alias>`;
   - SBOM and third-party notices stay in CI artifacts only, never on the
     release (a sidecar without its body is a dangling reference).
4. Deploy per PC: install the MSI once (or unpack the portable zip), point
   the relay directory at the same share path. Updates = install the new
   MSI (no in-app updater, by design).

Workflows: `.github/workflows/ci.yml` (acceptance on every push: schema
contract gate + full WinUI native build) and `.github/workflows/release.yml`
(tag-triggered). Rivet is pinned per release (`RIVET_PIN`, single source).
`scripts/check-release-version.sh` is the release preflight: the tag must
be annotated and match both `rivet.rktd` and the generated client's
embedded version.

## Building from source

Prerequisites:

- **Racket 9.x CS** (with `raco`)
- **rivet package**: `raco pkg install --auto rivet`
- **MSVC toolchain** (needed only for local builds; CI releases skip this):

  ```bat
  winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  ```

  (One UAC prompt; afterwards `raco rivet doctor` reports a complete
  toolchain.)

Build and run:

```bash
raco rivet dev      # build + launch
raco rivet package  # development distributable
```

## Tests

```bash
raco test app/backend.rkt                                   # pure unit tests
racket -e '(require (submod "app/backend.rkt" test)) (test-main)'   # full mailbox chain on the real share
```

The end-to-end self-test uses a dedicated `.shuttle-selftest` mailbox
(fake peer identity send → local receive + verify → remote cleanup), never
touches real transfers, and cleans up after itself. It requires the share
to be mapped, so CI runs only the pure units.

## Design notes

- **Mailbox protocol**: each transfer creates one subfolder
  (`yyyyMMdd-HHmmss-<id>`) written in order: `manifest.json` first (crash
  attribution), `payload.part` renamed to `payload` after the copy,
  `shuttle.done` last. Receivers act only on folders with `.done`, verify
  the size after copying, and delete the remote folder only after the local
  copy is fully on disk — share deletion bypasses the recycle bin.
- **Permission self-heal**: department-share public folders commonly allow
  create-but-not-delete for regular domain accounts. After creating its
  transfer folder, Shuttle immediately grants itself
  `icacls <folder> /grant "<account>:(OI)(CI)F"` (never `/t` — the
  (OI)(CI) inheritance propagates the whole subtree server-side). The
  creator is the owner, and owners can always grant themselves more, so
  cleanup just works anywhere with write access.
- **Cleans up only its own**: the startup/hourly sweep removes solely
  folders whose manifest names this machine as sender (crashed sends after
  a day, completed sends after a week) and never touches anyone else's.
- **Replay protection**: processed transfer ids go into `consumed.jsonl`.
- **Health at a glance**: a red/green dot mirrors mailbox reachability.

Stack: Racket backend (all product logic) + WinUI 3 native host
(presentation and OS integration) over [rivet](https://github.com/turlamai/rivet)
typed RPC.

## Repository layout

```
app/backend.rkt        Racket engine + RVT1 contract (mailbox, polling, icacls, config, history)
windows/               WinUI 3 host (XAML + C++/WinRT: drag-drop, list, tray, settings)
shared/i18n/           UI string single source (zh/en)
assets/app.ico         application icon
scripts/               release preflight
docs/acceptance/       acceptance record + share-permission evidence
```
