# The Shuttle mailbox protocol

Shuttle moves files between Windows machines through a **mailbox folder**
on a shared network drive. No daemons, no open ports, no central server:
the shared drive *is* the transport, and the protocol is a set of
file-layout conventions that make concurrent, multi-user, crash-tolerant
delivery possible with nothing but SMB reads and writes.

This document specifies the protocol precisely enough to implement a
compatible client. The reference implementation is this repository
(Racket core in `app/backend.rkt`).

## Participants and identity

- Every installation has an **identity**: a non-empty string, unique per
  machine in a sharing group. It defaults to `%COMPUTERNAME%` and is
  configurable in Settings.
- Any number of installations may share one mailbox. Every transfer is
  **addressed to exactly one recipient identity**.

## Mailbox layout

The relay directory (the "mailbox") holds one folder per transfer:

```
<relay-dir>/
  20261029-195740-45d9da2f/     # <yyyyMMdd-HHmmss>-<random 8 hex>
    manifest.json
    payload                     # exact bytes of the file
    shuttle.done                # empty marker, written LAST
```

### manifest.json

Written **first**, before any payload bytes, so a crashed send is still
attributable (the sweep reports it, and the original sender can clean it
up):

```json
{
  "shuttle": 1,
  "sender": "ALPHA-PC",
  "to": "BETA-PC",
  "file": "report.zip",
  "size": 12345,
  "id": "45d9da2f",
  "stamp": "2026-10-29 19:57:40"
}
```

| field    | meaning                                             |
|----------|-----------------------------------------------------|
| shuttle  | protocol version, currently `1`                     |
| sender   | sending identity                                    |
| to       | recipient identity; `""` means legacy broadcast     |
| file     | original file name (sanitized on the sender side)   |
| size     | payload size in bytes; receivers verify it          |
| id       | transfer id; consumed ids are never re-downloaded   |
| stamp    | sender-local timestamp, informational               |

### Write order (three-phase commit)

1. `manifest.json` — crash attribution.
2. `payload.part` → renamed to `payload` after the copy completes.
   A `payload.part` visible in the mailbox means the sender is still
   working (or died mid-copy).
3. `shuttle.done` — the commit marker. **Receivers must ignore a folder
   without it**, even if the payload looks complete.

There is no rename-into-mailbox trick available on SMB (cross-directory
renames within the same share are atomic, but the payload may be large
and the sender wants progress), so the marker protocol above is the
concurrency story: readers act only on committed folders, and the sender
never mutates a folder after writing `.done`.

## Claiming and cleanup

- A receiver claims a folder only when **all** hold:
  - `shuttle.done` exists,
  - the transfer id is not in its consumed ledger
    (`consumed.jsonl` — replay protection across restarts),
  - the `sender` is not itself,
  - the manifest is **addressed to it**: `to` equals its identity
    (case-insensitive) — or `to` is absent/empty, which is the legacy
    v0.3 pair semantics kept for migration.
- The receiver copies `payload` to a non-clobbering local path
  (`name (2).ext` on collision), verifies the byte count against
  `size`, and only then deletes the remote folder. **A size mismatch
  discards the local copy and keeps the folder.**
- Network-share deletion is permanent (no recycle bin); nothing is ever
  deleted on a failed or unverified receive.

### Permission self-heal

Department shares are often configured so ordinary users may create
folders but not delete them. The creating account is always the owner
and owners can re-grant themselves, so a receiver that fails to delete
applies `icacls <folder> /grant "<own account>:(OI)(CI)F"` (the
inheritance flags propagate the whole subtree server-side; never `/t`)
and retries — lazily, on the first failed delete, hidden via a WMI
server-side process create.

### Sweeps

Each installation periodically removes folders whose manifest names
**itself** as sender: crashed sends (no `.done`) after 24 hours,
completed-but-unclaimed sends after 7 days. Nothing belonging to another
identity is ever touched.

## Compatibility

| manifest              | v0.4+ receiver                    | v0.3 receiver      |
|-----------------------|-----------------------------------|--------------------|
| `to` = their identity | claims                            | claims (ignores `to`) |
| `to` = other identity | ignores                           | **claims — do not mix versions on a shared mailbox** |
| `to` absent/empty     | claims (legacy broadcast)         | claims             |

Run v0.4+ on every installation that shares a mailbox.
