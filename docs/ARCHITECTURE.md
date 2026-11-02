# Architecture

Shuttle is a three-layer desktop application: a **Racket core** owns all
product logic, a **WinUI 3 host** owns presentation and OS integration,
and a **mailbox folder on a network share** is the entire transport. The
layers communicate over Rivet's typed RPC protocol (RVT1) with an
embedded Racket runtime — there is no second process, no socket, and no
web view.

```
┌──────────────────────────── Windows process ───────────────────────────┐
│                                                                        │
│  WinUI 3 host (C++/WinRT)            Racket core (embedded CS runtime) │
│  ┌──────────────────────┐  RVT1 over  ┌─────────────────────────────┐  │
│  │ drop zone / list     │  typed RPC  │ mailbox protocol            │  │
│  │ settings + tray      │◄───────────►│ claim rules · sweeps        │  │
│  │ picker · toasts      │  events ↑   │ icacls self-heal · config   │  │
│  └──────────────────────┘  (typed)   └──────────────┬──────────────┘  │
│                                                     │ SMB              │
│                                              ┌──────▼──────┐           │
│                                              │ mailbox dir │           │
│                                              │ on Z:\ share│           │
│                                              └─────────────┘           │
└────────────────────────────────────────────────────────────────────────┘
         ▲                                                     ▲
         │ same layout on the peer machine                      │
```

The full protocol (folder layout, manifest fields, claim rules, crash
tolerance) is specified in [PROTOCOL.md](PROTOCOL.md).

## Why this shape

**All product logic in Racket.** Claim rules, retries, sanitization,
ledger bookkeeping, and the permission self-heal are pure, testable
functions over paths and JSON — `raco test` covers them without a GUI,
and most of them were hardened by unit tests before they ever met a
user machine. The host contains no business logic it could get wrong.

**The share is the transport.** Both machines already trust and mount
the same file server; the mailbox protocol adds only ordering markers
on top of it. No ports, no discovery, no pairing handshake — two
installations that point at the same directory are paired.

**Fail-safe by construction.** Every destructive step is gated: a
receiver deletes remote state only after its local copy is fully
written and size-verified; the sweeper only touches folders attributed
to the local machine; the permission self-heal only ever grants the
current user more access to a folder that user created.

## Module map

| path                | owns                                                    |
|---------------------|---------------------------------------------------------|
| `app/backend.rkt`   | RVT1 contract, mailbox engine, sweeps, config, updates  |
| `windows/`          | WinUI 3 host: drop zone, transfer list, tray, settings  |
| `shared/i18n/`      | UI string single source (zh/en), embedded at build      |
| `scripts/`          | release preflight (version contract)                    |
| `.github/workflows/`| CI (schema gate + native build) and signed releases     |

## Key invariants

1. `manifest.json` is written before any payload byte; `shuttle.done`
   is written last. Nothing mutates a folder after `.done`.
2. A receiver deletes a remote folder only after its local copy is
   fully written and size-verified. Share deletion bypasses the
   recycle bin.
3. The permission self-heal grants only the current user more access
   to a folder that user created, and runs lazily on the first failed
   delete — never proactively at send time.
4. Consumed transfer ids persist (`consumed.jsonl`); a transfer is
   downloaded at most once per installation.
5. The sweeper touches only folders whose manifest names the local
   machine as sender.
