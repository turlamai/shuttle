# Security policy

## Trust model

Shuttle's security rests on two independent mechanisms:

**Update channel.** The `update-stable.json` manifest is signed with an
Ed25519 key whose public half is embedded in the application. The
updater verifies the signature and key id **before parsing the
payload**, then checks application identity, channel, version
precedence, and the artifact's URL, byte size, and SHA-256 before
downloading. A failed or incomplete download is discarded. If any step
fails, the updater surfaces the error and offers the Releases page —
unauthenticated input never reaches the manifest parser or the
installer.

**Transport placement.** Files travel through a mailbox folder on a
network share. They are readable in transit by anyone with share
access — the same trust level as the share itself. Do not point the
relay directory at a share readable by people who should not read your
files.

## Known limitations

- The transport offers no end-to-end encryption or sender
  authentication beyond share permissions; anyone with write access to
  the mailbox can drop a file addressed to any recipient. Receivers
  verify size, not authorship.
- Windows binaries are not Authenticode-signed (development release
  policy); the updater verifies them by the signed manifest's SHA-256
  instead of a publisher certificate.

## Reporting a vulnerability

Open a private security advisory via GitHub (Security → Advisories) or
contact the maintainer directly. Please include reproduction steps and
the affected version.
