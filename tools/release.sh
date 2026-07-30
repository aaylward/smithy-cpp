#!/usr/bin/env bash
# Release helpers shared by //tools:release_test and the release workflow, so
# the logic the tag-push workflow depends on is exercised by every PR instead
# of first running on tag day.
#
#   release.sh check            assert the version sources agree
#   release.sh notes X.Y.Z      print the CHANGELOG section for a version
#   release.sh version          print the version the tree declares
#   release.sh bump X.Y.Z-dev   open the next development cycle
#
# Run from the repo root (the sh_test runs from the runfiles root, where the
# same relative paths resolve).
set -euo pipefail

VERSION_CC=runtime/src/core/version.cc
CONFIG_H=runtime/include/smithy/client/config.h
GRADLE_PROPERTIES=codegen/gradle.properties
CHANGELOG=CHANGELOG.md

die() {
  echo "release.sh: $*" >&2
  exit 1
}

# smithy::Version()'s literal is the single source of truth; the other two
# files mirror it (docs/versioning.md).
read_version() {
  sed -n 's/^std::string_view Version() { return "\(.*\)"; }$/\1/p' "$VERSION_CC"
}

read_user_agent() {
  sed -n 's/^  std::string user_agent = "\(.*\)";$/\1/p' "$CONFIG_H"
}

read_gradle_version() {
  sed -n 's/^version=\(.*\)$/\1/p' "$GRADLE_PROPERTIES"
}

# The first "## [" line: [Unreleased] while developing, [X.Y.Z] once cut.
read_changelog_heading() {
  grep -m1 '^## \[' "$CHANGELOG" || true
}

check() {
  local version user_agent gradle_version heading
  version=$(read_version)
  [[ -n $version ]] || die "no version literal in $VERSION_CC"

  user_agent=$(read_user_agent)
  [[ $user_agent == "smithy-cpp/$version" ]] ||
    die "$CONFIG_H has user_agent '$user_agent', expected 'smithy-cpp/$version'"

  gradle_version=$(read_gradle_version)
  [[ $gradle_version == "$version" ]] ||
    die "$GRADLE_PROPERTIES has version '$gradle_version', expected '$version'"

  heading=$(read_changelog_heading)
  [[ -n $heading ]] || die "no '## [...]' section in $CHANGELOG"

  if [[ $version == *-dev ]]; then
    [[ $heading == "## [Unreleased]" ]] ||
      die "version is $version but $CHANGELOG opens with '$heading'; a -dev version needs '## [Unreleased]'"
  else
    [[ $heading =~ ^##\ \[$version\]\ -\ [0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] ||
      die "version is $version but $CHANGELOG opens with '$heading'; expected '## [$version] - YYYY-MM-DD'"
    grep -qE "^\[$version\]: " "$CHANGELOG" ||
      die "$CHANGELOG has no '[$version]:' link footnote"
  fi

  echo "release.sh: $version consistent across $VERSION_CC, $CONFIG_H, $GRADLE_PROPERTIES, $CHANGELOG"
}

notes() {
  local version=${1-}
  [[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
    die "notes needs a released X.Y.Z version, got '${version:-<none>}'"

  local section
  # Everything after the heading, stopping at the next section or the link
  # footnotes. $(...) drops the trailing blank lines for free.
  section=$(awk -v heading="## [$version] - " '
    index($0, heading) == 1 { inside = 1; next }
    inside && (/^## \[/ || /^\[[^]]+\]: /) { exit }
    inside && !started && /^[[:space:]]*$/ { next }
    inside { started = 1; print }
  ' "$CHANGELOG")

  [[ -n $section ]] || die "no notes for $version in $CHANGELOG"
  printf '%s\n' "$section"
}

# In-place edit without sed -i, whose syntax differs between BSD and GNU.
rewrite() {
  local file=$1 expression=$2 tmp
  tmp=$(mktemp "${TMPDIR:-/tmp}/release.XXXXXX")
  sed "$expression" "$file" >"$tmp"
  mv "$tmp" "$file"
}

# Reopens the CHANGELOG and moves the three version strings on to the next
# development version. Edits the tree only — review the diff and commit it
# yourself.
bump() {
  local target=${1-}
  [[ $target =~ ^[0-9]+\.[0-9]+\.[0-9]+-dev$ ]] ||
    die "bump needs an X.Y.Z-dev version, got '${target:-<none>}'"

  local current heading
  current=$(read_version)
  [[ -n $current ]] || die "no version literal in $VERSION_CC"
  [[ $current != *-dev ]] ||
    die "already developing $current; bump runs once, right after a release"

  heading=$(read_changelog_heading)
  [[ $heading =~ ^##\ \[$current\]\ - ]] ||
    die "$CHANGELOG opens with '$heading', expected the closed '## [$current] - ...' section"

  rewrite "$VERSION_CC" \
    "s|^std::string_view Version() { return \"[^\"]*\"; }$|std::string_view Version() { return \"$target\"; }|"
  rewrite "$CONFIG_H" \
    "s|^  std::string user_agent = \"smithy-cpp/[^\"]*\";$|  std::string user_agent = \"smithy-cpp/$target\";|"
  rewrite "$GRADLE_PROPERTIES" "s|^version=.*$|version=$target|"

  # A fresh [Unreleased] above the section just released; the released one and
  # its link footnote stay untouched.
  local tmp
  tmp=$(mktemp "${TMPDIR:-/tmp}/release.XXXXXX")
  awk '
    !done && /^## \[/ { print "## [Unreleased]"; print ""; done = 1 }
    { print }
  ' "$CHANGELOG" >"$tmp"
  mv "$tmp" "$CHANGELOG"

  # The bump is only done if the tree it produced satisfies the same guard
  # every PR runs.
  check
  echo "release.sh: bumped $current -> $target; review the diff and commit"
}

case "${1-}" in
  check) check ;;
  notes) notes "${2-}" ;;
  bump) bump "${2-}" ;;
  version)
    version=$(read_version)
    [[ -n $version ]] || die "no version literal in $VERSION_CC"
    printf '%s\n' "$version"
    ;;
  *) die "usage: release.sh check | notes X.Y.Z | version | bump X.Y.Z-dev" ;;
esac
