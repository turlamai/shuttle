#!/usr/bin/env bash
# Release preflight: the tag is the only input; rivet.rktd and the
# generated native client must carry the same version. Release tags must
# be annotated — lightweight tags have slipped through on other products
# and the family convention requires the tag message to travel with the ref.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="${1:-}"

fail() {
  echo "release preflight: $*" >&2
  exit 1
}

[[ -n "$TAG" ]] || fail "no tag given"
[[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.-]+)?$ ]] || \
  fail "tag '$TAG' is not a v-prefixed semantic version"
VERSION="${TAG#v}"

# Release tags must be annotated. CI checkouts may not carry the tag
# object, so fetch the real ref explicitly first; locally a no-op.
git -C "$ROOT" fetch -q --no-tags --force origin "refs/tags/$TAG:refs/tags/$TAG" 2>/dev/null || true
[[ "$(git -C "$ROOT" cat-file -t "$TAG" 2>/dev/null)" == "tag" ]] || \
  fail "tag '$TAG' is lightweight — release tags must be annotated (git tag -a)"

# The Rivet app manifest carries the product version on this tree.
RIVET_RKTD_VERSION="$(grep -oE '\(version \.[[:space:]]*"[^"]+"' "$ROOT/rivet.rktd" | grep -oE '"[^"]*"' | tr -d '"')"
[[ "$RIVET_RKTD_VERSION" == "$VERSION" ]] || \
  fail "rivet.rktd version '$RIVET_RKTD_VERSION' does not match tag '$TAG'"

# The generated native client embeds the same release identity. The file
# is marked do-not-edit: a mismatch means the tree forgot to regenerate
# after the version bump.
GENERATED_VERSION="$(grep -oE 'kVersion\[\] = "[^"]*"' "$ROOT/windows/GeneratedBackend.hpp" | grep -oE '"[^"]*"' | tr -d '"')"
[[ "$GENERATED_VERSION" == "$VERSION" ]] || \
  fail "windows/GeneratedBackend.hpp kVersion '$GENERATED_VERSION' does not match tag '$TAG' (run raco rivet build, do not hand-edit)"

# The backend's update/identity constant joins the version contract.
grep -qF "(define app-version \"$VERSION\")" "$ROOT/app/backend.rkt" ||   fail "app/backend.rkt app-version does not match tag '$TAG'"

# The updater identity must equal rivet.rktd's identifier: signed update
# manifests carry that application_id and select-update rejects any
# mismatch, silently degrading in-app updates to the releases page.
RIVET_RKTD_IDENTIFIER="$(grep -oE '\(identifier \.[[:space:]]*"[^"]+"' "$ROOT/rivet.rktd" | grep -oE '"[^"]*"' | tr -d '"')"
[[ -n "$RIVET_RKTD_IDENTIFIER" ]] || fail "rivet.rktd has no identifier"
grep -qF "(define update-application-id \"$RIVET_RKTD_IDENTIFIER\")" "$ROOT/app/backend.rkt" || \
  fail "app/backend.rkt update-application-id does not match rivet.rktd identifier '$RIVET_RKTD_IDENTIFIER'"

echo "release preflight: $TAG ok"
