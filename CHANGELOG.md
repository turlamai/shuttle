# Changelog

## [1.2.3]

### Added

- **In-app updates, hands-free**: Shuttle now checks for updates in the
  background (at most once per 4 hours; a switch in Settings turns the
  silent check off), and a newer version offers to download and install
  itself in one click — with live progress, a cancellable download, and
  the verified offer kept for retry. No more watching the Releases page.
- **Portable installs update in place**: the zip flavor now participates
  in online updates — the verified archive swaps the install directory
  automatically (with a fallback copy kept until the new version
  launches), matching the MSI path.
- **Install failures report back**: a failed update attempt surfaces on
  the next launch with the underlying reason instead of vanishing, and
  every error bar carries a 复制错误 button that puts the verbatim
  message on the clipboard for bug reports.

### Changed

- Releases now ship with update-chain evidence — signed-feed rejection,
  the download lifecycle with data preservation, and install-flavor
  artifact selection — run by the release pipeline on the exact
  published tree (see docs/UPDATE.md).

## [1.2.2]

### Fixed

- **Dragging a folder no longer reports "文件夹不存在"**: a live
  directory failed the sender's file-only existence check and surfaced
  as a missing-file error for a folder that was right there. Folder
  drops are now intercepted up front with a clear "暂不支持发送文件夹"
  message (compress to zip first), and the sender names that remedy
  instead of claiming the path is missing.

### Note

- Copies at 1.2.1 can update to this release in-app. Copies at 1.2.0 or
  older still carry the broken updater and need this one manual
  install; every later update arrives in-app.

## [1.2.1]

### Fixed

- **In-app updates actually engage now**: two faults broke every update
  check — the updater presented an identity that signed manifests don't
  carry, and the update public key was embedded in the wrong format — so
  Shuttle always fell back to opening the releases page instead of
  updating in place. Checking for updates now downloads the signed MSI,
  verifies it against the manifest, and installs it.
  **Note:** copies at 1.2.0 or older carry the broken updater, so
  reaching 1.2.1 is the one last manual install; from here on, updates
  arrive in-app.
- **Settings paths are selectable**: the relay/download `…` buttons open
  a real folder picker (they were dead buttons before), and the
  interface language selector — shipped invisible in 1.2.0 — is now
  shown in Settings.

## [1.2.0]

### Added

- **Received files open in one click**: completed received rows show an
  "打开所在文件夹" hint; clicking the row opens the containing folder
  with the file selected. Receive toasts now state where files were
  saved.
- **The drop zone states the recipient inline**: "拖放文件发给
  <target>" once a target is set, or "先填写对方的电脑名" when it is
  not — no manual reading required to understand the flow. The target
  placeholder now explains that the name is visible in the peer's
  Shuttle title bar, and Settings shows the same hint under the machine
  name field.

### Changed

- The default relay directory is a generic `Z:\Shared\Shuttle`
  (config overrides persist); setup docs updated accordingly.

## [1.1.0]

### Added

- **The UI now follows the system language** (zh/en); the remaining
  hardcoded strings (caption prefix, download progress) joined the i18n
  single source.
- **Event-driven transfer list**: transfers update live from backend
  events (worker-thread emission, rivet #364); the slow timer now only
  reconciles.

### Changed

- `RIVET_PIN` bumps to rivet main, including fail-closed signed-feed
  validation (#378) and multi-installer feed composition (#380).
- Documentation set: mailbox protocol spec, architecture, contributing
  guide (including the UWP-component toolchain trap), security policy;
  public README gains a sanitized production screenshot.

## [1.0.0] — first public release

### Added

- **Recipient addressing**: every installation has an identity (machine
  name by default, customizable in Settings) and every send names its
  recipient; receivers claim only mail addressed to them. Multiple
  people can share one relay directory without cross-delivery, and
  colleague-to-colleague sends work alongside pair usage.
- **In-app updates** (MSI installs): signed-manifest update channel with
  download progress and install-and-restart; portable copies fall back
  to the Releases page.

### Fixed

- A long tail of real-machine findings: sender-thread queue arity,
  RPC result typing, startup crash after the first transfer, the
  permission self-heal running in-process instead of shelling out, and
  a UI freeze caused by events from backend threads being dropped
  (the list is reconciled by polling).

## [0.4.0]

### Added

- **Recipient addressing** — the mailbox now supports multiple people
  sharing one relay directory. Every installation has an identity (the
  machine name by default, customizable in Settings) and every send
  names its recipient from the "Send to" box on the main window
  (previous recipients remembered; the list auto-fills from machines
  seen on the mailbox). Receivers claim only mail addressed to them;
  mail for others is left untouched. Legacy unaddressed mail (v0.3
  senders) is still claimed by any receiver for pair compatibility.
- Settings gains a "machine name" field; the send flow blocks with an
  inline hint when no recipient is set.

### Changed

- Multi-user deployments no longer need per-pair relay directories:
  everybody shares one mailbox and addressing keeps deliveries separate.

## [0.3.2]

### Fixed

- The install-flavor detection now also scans the HKCU uninstall key, so
  per-user MSI installs are recognized and receive the update channel.

## [0.3.1]

### Fixed

- A failed update check (portable install, network down) now shows the
  reason and opens the Releases page instead of failing silently.

## [0.3.0]

### Fixed

- The update button works end to end: the settings dialog's update block
  is now a member-function state machine (check -> download with percent
  -> install and restart via MsiHandoff), with completions dispatched to
  the UI thread before touching controls. When the update channel is
  unreachable (private repository manifests need authentication the
  updater does not carry), the app opens the Releases page instead of
  showing a bare error.

## [0.2.0]

### Added

- **In-app updates** (MSI installs). Settings gains an update block:
  check, download with progress, and install-and-restart, all driven by
  rivet's signed update channel — manifests are Ed25519-verified against
  an embedded key before parsing, artifacts are size- and hash-checked,
  and the Windows host hands the verified MSI to msiexec and relaunches.
  The installer flavor is detected from the registry at startup; portable
  copies keep updating by re-downloading. The release pipeline now signs
  and publishes `update-stable.json` alongside the installers.

## [0.1.9]

### Fixed

- **No more black window when sending.** The send-time permission grant
  (however hidden) still surfaced a terminal on Windows 11's delegated
  default-terminal setup. It is no longer needed at send time: the
  creating account is the folder owner and owners can always re-grant
  themselves, so the grant now happens **lazily at delete-failure**
  (receiver cleanup and sweep), hidden via a WMI server-side process
  create with ShowWindow=0 that no delegation overrides. On the default
  share deletion just works and the grant never runs — sends are
  completely child-process-free.

## [0.1.8]

### Fixed

- **Sends stayed stuck at "Queued" — the third and final round.**
  v0.1.7's in-process FFI grant was the wrong cure: Racket CS runs FFI
  calls on the original thread, which the WinUI host occupies with its
  message loop, so any grant from a background thread waits forever.
  Diagnosed by step-timed repro (grant completes in 5 ms from a console
  REPL whose original thread is idle) plus the leftover empty mailbox
  folders and the new per-step diagnostic log. The grant now shells out
  to icacls hidden behind **wscript** (a GUI-subsystem host: spawning it
  allocates no console, and the .js drives it with window style 0),
  every external call runs under a hard 5-10 second timeout, and each
  step writes `%APPDATA%\Shuttle\send-debug.log` so a hung send leaves
  its exact stopping point on disk.
- **The transfer list froze at "Queued" even on success.** The backend
  event emitter lives on the RPC thread, so events from the sender and
  receiver threads were silently dropped and the UI never progressed.
  The list is now reconciled against the authoritative table by a 1.5 s
  poll (also covers history clears and incoming transfers).

## [0.1.7]

### Fixed

- **Sending hung forever at "Queued" with a black terminal window.**
  The v0.1.5 attempt to run the icacls permission grant inside a
  headless conhost hung the sender thread on Windows 11 (whose default
  terminal delegation does not honor the flag) and popped a black
  window titled after the host process. The grant now runs **in
  process** through advapi32's ACL API
  (GetNamedSecurityInfoW / SetEntriesInAclW / SetNamedSecurityInfoW):
  no child process, no window, no hang, and the same (OI)(CI)F result
  verified against the real share. A plain icacls call remains as the
  fallback if the FFI module is ever unavailable.

## [0.1.6]

### Fixed

- **The app errored on every launch after a completed transfer**
  ("hash-ref: no value found for key: 'stamp"). The ledger written by
  each transfer uses the ledger schema (file/ts/msg) but was loaded
  verbatim into the transfer table whose consumers expect the UI
  schema (filename/stamp/message) — the first restart after any real
  transfer crashed list-transfers. Ledger rows are now normalized into
  proper entries on load (status derived from ok, stamp from ts), rows
  without an id are skipped, and the sort key defaults defensively.
  Reproduced locally with the exact user-visible error before the fix;
  a regression test feeds real ledger rows through the startup path.

## [0.1.5]

### Fixed

- **Sending failed with "expected Transfer received Unsupported".** The
  send-file RPC returned the raw internal entry map while its declared
  result type is a Transfer record, so the wire layer rejected every
  send at the last step — often after the transfer had actually been
  queued. The RPC body now returns a proper Transfer record, is
  factored out for direct testing, and a regression test asserts the
  result type.
- **No more black terminal flashes on every send.** The icacls
  self-heal is a console child; spawned from the windowed host it
  popped a visible terminal per send. It now runs inside a headless
  conhost (plain fallback on systems without the flag).

### Added

- README guidance for multi-user deployments: Shuttle is a pair tool;
  each pair of machines must use its own relay directory.

## [0.1.4]

### Fixed

- **Sending actually works.** The sender thread pulled only the transfer
  id off the queue while the send routine takes id + path, so every
  send died on an arity error before a byte left the machine — a
  regression the receive-only self-test could not see. The queue now
  carries (id . path), the sender loop tolerates a malformed task
  (fails that transfer, never the thread), and a second self-test
  drives the real queue+thread path end to end on the share.

## [0.1.3]

### Changed

- **Settings moved out of the title bar.** The gear button competed with
  the system Min/Max/Close corner — no amount of spacing makes that
  pattern feel native. The caption bar now carries identity only
  (product name + machine), and the settings entry lives in the footer
  status row (health dot left, gear right), matching compact utility
  conventions. The DPI-inset machinery from 0.1.1/0.1.2 is gone with it.

## [0.1.2]

### Fixed

- **Caption polish at high DPI.** The gear inset now converts the
  system caption-button width from physical pixels to DIPs (it was
  double-scaled on >100% displays), and the gear aligns to the top
  32-DIP system caption-button band instead of floating low.

## [0.1.1]

### Fixed

- **The settings gear no longer overlaps the system Min/Max/Close
  buttons.** With content extending into the title bar, the caption bar
  now reserves the system caption-button inset (read live per DPI, with a
  sane pre-layout fallback) so the gear sits left of the buttons instead
  of underneath them; the system buttons' chrome is blended transparent
  into the Mica backdrop.
- **Compact utility-window default size.** The window now opens at
  460×660 logical pixels (DPI-scaled) instead of the platform default.

## [0.1.0] — initial release

- **Mailbox-style direct transfer**: drop a file on machine A; it travels
  through the relay mailbox on the share (default
  `Z:\\Shared\\Shuttle`) and lands in
  `Downloads\Shuttle` on machine B with a Windows toast; mailbox residue is
  cleaned up automatically.
- **Permission self-heal**: every transfer folder is immediately granted
  `icacls (OI)(CI)F` (never `/t` — inheritance propagates the whole subtree
  server-side), so cleanup works even in create-but-not-delete share
  folders. Only folders owned by this machine are ever swept (crashed
  sends after 1 day, completed sends after 7 days), and the remote folder
  is deleted only after the local copy is fully written and verified
  (share deletion bypasses the recycle bin).
- **Fail-safe protocol**: `manifest.json` is written first (crash
  attribution), the payload is copied as `payload.part` and renamed,
  `shuttle.done` lands last; receivers act only on complete transfers,
  discard size-mismatched local copies, and record consumed ids to prevent
  redownload loops.
- **Native UI**: Mica backdrop with a custom caption bar, drop zone
  (drag highlight / click to pick), transfer list with incremental
  progress, tray residency (menu, toasts, Explorer-restart survival),
  settings (relay/download directories, sign-in autostart), and a
  red/green mailbox-health dot.
- **Engineering**: Racket backend (all product logic) + WinUI 3 host over
  rivet typed RPC; zh/en UI strings from a single source; unreachable-share
  warning; tag-driven CI releases with checksum sidecars and stable
  download aliases.
