#!/bin/bash
# SessionStart hook for Claude Code on the web: make `bazel test` work
# behind the sandbox's egress proxy. The proxy admits GitHub *release
# assets* (/releases/download/...) and git-over-HTTPS but 403s the
# on-demand source-archive endpoints (/archive/..., codeload.github.com);
# bazel/make-git-overrides.sh rebuilds the archive-URL modules from git
# clones plus BCR metadata, and .bazelrc.user points Bazel at them. See
# docs/development.md, "Machine-specific Bazel flags".
set -euo pipefail

# Web sessions only: local machines fetch archives fine and should never
# have their .bazelrc.user touched.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

cd "$CLAUDE_PROJECT_DIR"

# bazelisk (reads .bazelversion) via npm; the bazel binary itself downloads
# from GitHub release assets, which the proxy admits.
command -v bazel >/dev/null 2>&1 || npm install -g @bazel/bazelisk

# jq: make-git-overrides.sh parses BCR source.json with it.
command -v jq >/dev/null 2>&1 || {
  apt-get update -qq && apt-get install -y -qq jq
} >/dev/null

# Rebuild the blocked-archive modules from git (idempotent: existing
# checkouts are kept, so the cached container skips straight through).
bazel/make-git-overrides.sh "$HOME/bazel-overrides"

# Wire the overrides in. .bazelrc.user is gitignored and try-imported by
# .bazelrc; --lockfile_mode=off keeps override runs from dirtying the
# checked-in MODULE.bazel.lock. Written whole: on the web this file
# belongs to the hook.
cat > .bazelrc.user <<EOF
common --lockfile_mode=off
import $HOME/bazel-overrides/overrides.bazelrc
EOF

echo "bazel sandbox setup complete: $(grep -c override_module "$HOME/bazel-overrides/overrides.bazelrc") module overrides active"
