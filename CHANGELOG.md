# Changelog

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
  `Z:\19 汽车电子事业部\研发部\04_Software\LiDAR\.shuttle`) and lands in
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
