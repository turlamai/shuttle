---
name: Bug report
about: Something in Shuttle is broken or behaves differently from the README
title: ''
labels: bug
assignees: ''
---

**Describe the bug**

A clear description of what happened and what you expected instead.
中文描述完全没问题 — 写清楚即可。

**To reproduce**

Steps that trigger the behavior (e.g. dropped file X, receiver never
notified; or relay directory probing warning).

**Environment**

- Platform: Windows 10 / 11 (build)
- Shuttle version: (Settings ⚙ dialog title, or the release tag)
- Install shape: MSI / portable zip
- Relay directory: (default LiDAR mailbox, or a custom share path)

**Transfer context**

- Direction: send / receive
- The row in the transfer list (status + message), if any
- Share state at the time: mapped drive letter reachable? Any
  Access-Denied dialogs from Explorer around the same moment?

**Screenshots / logs**

If applicable, add screenshots. `%APPDATA%\Shuttle\history.jsonl` holds
the transfer ledger for the exact transfer id.
