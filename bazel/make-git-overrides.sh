#!/usr/bin/env bash
# Rebuilds, from git clones, the modules whose source archives a
# download-blocking proxy refuses (docs/development.md, "Machine-specific
# Bazel flags"). Some proxies allow GitHub *release assets*
# (/releases/download/...) and git-over-HTTPS but 403 the on-demand
# source-archive endpoints (/archive/..., codeload.github.com). Only the
# archive-URL modules need help: this script finds them in
# MODULE.bazel.lock, clones each at its pinned tag, replays the BCR's
# patches and overlay files from bcr.bazel.build (metadata is not blocked),
# and emits an `--override_module` line per module into
# <dest>/overrides.bazelrc.
#
# Usage:   bazel/make-git-overrides.sh [dest-dir]   # default ~/bazel-overrides
# Then:    echo "import $HOME/bazel-overrides/overrides.bazelrc" >> .bazelrc.user
#          echo "common --lockfile_mode=off" >> .bazelrc.user
#
# --lockfile_mode=off is required: overridden modules drop out of lockfile
# verification, and without it every run dirties the checked-in
# MODULE.bazel.lock. Re-run the script after a dep bump; it is idempotent
# and skips modules already built.
set -euo pipefail

DEST="${1:-$HOME/bazel-overrides}"
REGISTRY="https://bcr.bazel.build"
LOCKFILE="$(dirname "$0")/../MODULE.bazel.lock"
RC="$DEST/overrides.bazelrc"
mkdir -p "$DEST"
: > "$RC.tmp"

command -v jq >/dev/null || { echo "jq is required" >&2; exit 1; }

# Modules that toolchain registration fetches during analysis without the
# lockfile ever recording a source.json for them (rules_perl arrives via
# the openssl module's toolchains). The lockfile scan below cannot see
# these, so they are pinned here; if a build still 403s on a module this
# script didn't cover, add it and re-run.
EXTRA_MODULES="rules_perl/0.5.0"

# Every module version the lockfile consulted a source.json for, plus the
# extras above.
modules=$(
  { grep -oE '"https://bcr\.bazel\.build/modules/[^"]+/source\.json"' "$LOCKFILE" |
      sed 's|.*/modules/||; s|/source\.json"||'
    printf '%s\n' $EXTRA_MODULES; } | sort -u)

for mod in $modules; do
  name="${mod%%/*}" version="${mod#*/}"
  src="$(curl -fsS "$REGISTRY/modules/$mod/source.json")"
  url="$(jq -r .url <<<"$src")"
  # Release-asset and non-GitHub URLs download fine; only the on-demand
  # archive endpoints are blocked.
  case "$url" in
    *github.com/*/archive/*) ;;
    *) continue ;;
  esac
  out="$DEST/$name-$version"
  if [ ! -d "$out" ]; then
    # https://github.com/<org>/<repo>/archive/refs/tags/<tag>.{tar.gz,zip}
    repo="$(sed -E 's|https://github.com/([^/]+/[^/]+)/archive/.*|\1|' <<<"$url")"
    tag="$(sed -E 's|.*/archive/(refs/tags/)?(.*)\.(tar\.gz\|zip)|\2|' <<<"$url")"
    echo ">> $mod  <-  $repo @ $tag"
    git clone -q --depth 1 --branch "$tag" "https://github.com/$repo.git" "$out.tmp"
    rm -rf "$out.tmp/.git"
    # The BCR's patches (patch_strip applies to all of them)...
    strip="$(jq -r '.patch_strip // 0' <<<"$src")"
    for p in $(jq -r '(.patches // {}) | keys[]' <<<"$src"); do
      curl -fsS "$REGISTRY/modules/$mod/patches/$p" | patch -s -p"$strip" -d "$out.tmp"
    done
    # ...then overlay files (BUILD.bazel, ...) land on top...
    for f in $(jq -r '(.overlay // {}) | keys[]' <<<"$src"); do
      mkdir -p "$out.tmp/$(dirname "$f")"
      curl -fsS "$REGISTRY/modules/$mod/overlay/$f" -o "$out.tmp/$f"
    done
    # ...and the registry's MODULE.bazel is authoritative — for registry
    # modules Bazel injects it over whatever the archive carries (boost
    # upstreams carry none at all), so a local override needs it too.
    curl -fsS "$REGISTRY/modules/$mod/MODULE.bazel" -o "$out.tmp/MODULE.bazel"
    mv "$out.tmp" "$out"
  fi
  echo "common --override_module=$name=$out" >> "$RC.tmp"
done

mv "$RC.tmp" "$RC"
echo "wrote $RC ($(grep -c override_module "$RC") overrides)"
