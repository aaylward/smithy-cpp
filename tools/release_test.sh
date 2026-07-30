#!/usr/bin/env bash
# Guards the version sources against drift and pins the CHANGELOG extraction
# the release workflow pipes into `gh release create`. Runs in the normal
# `bazel test //...` matrix, so a half-finished version bump fails on the PR
# rather than at tag time.
set -euo pipefail

RELEASE=tools/release.sh
failures=0

fail() {
  echo "FAIL: $*" >&2
  failures=$((failures + 1))
}

expect_success() {
  local what=$1
  shift
  "$@" >/dev/null || fail "$what: expected success, got exit $?"
}

expect_failure() {
  local what=$1
  shift
  if "$@" >/dev/null 2>&1; then
    fail "$what: expected non-zero exit"
  fi
}

expect_success "check on the committed tree" "$RELEASE" check

# Smithy trait names are @-prefixed. Outside a code span GitHub renders them as
# user mentions, so the notes credit whoever owns that account: v0.1.0 shipped
# listing @required and @timestampFormat as contributors.
bare_mentions=$(sed 's/`[^`]*`//g' CHANGELOG.md | grep -n '@[A-Za-z]' || true)
if [[ -n $bare_mentions ]]; then
  fail "CHANGELOG.md has @-mentions outside code spans:
$bare_mentions"
fi

# Not a released version: the CHANGELOG heading exists while developing, but
# there are no notes to publish for it.
expect_failure "notes Unreleased" "$RELEASE" notes Unreleased
expect_failure "notes with no argument" "$RELEASE" notes
expect_failure "notes for an absent version" "$RELEASE" notes 99.99.99

# The workflow reads the version through this subcommand to decide whether the
# pushed tag names the commit it was pushed at.
version=$("$RELEASE" version)
[[ -n $version ]] || fail "version: printed nothing"

if [[ $version == *-dev ]]; then
  # Development state: the current version has no section of its own.
  expect_failure "notes for the -dev version" "$RELEASE" notes "$version"
else
  section=$("$RELEASE" notes "$version") || fail "notes $version: expected success"
  [[ -n ${section:-} ]] || fail "notes $version: empty section"
  # Stopping at the next heading is the whole contract — a greedy extraction
  # would ship every prior release's notes as the current release's.
  if grep -q '^## \[' <<<"${section:-}"; then
    fail "notes $version: ran into the next section"
  fi
  if grep -qE '^\[[^]]+\]: ' <<<"${section:-}"; then
    fail "notes $version: ran into the link footnotes"
  fi
fi

if ((failures > 0)); then
  echo "$failures assertion(s) failed" >&2
  exit 1
fi
echo "PASS"
