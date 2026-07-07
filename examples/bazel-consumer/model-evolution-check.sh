#!/usr/bin/env bash
# Scripted model-evolution loop (docs/model-evolution.md): evolve the todo
# model the way a consumer would on day 2 and verify the two properties the
# doc promises. Run from anywhere; it operates on this directory and restores
# the model when it exits. CI runs it after the plain consumer test.
#
#   1. Additive change — a new optional member is source-compatible: the
#      existing handler and integration test rebuild and pass untouched.
#   2. New operation — the generated handler interface grows a pure-virtual
#      method, so every handler implementation fails to compile until it is
#      updated: the compiler, not the wire, finds the unimplemented surface.
set -euo pipefail
cd "$(dirname "$0")"

bazel="${BAZEL:-$(command -v bazelisk || command -v bazel)}"
model=model/todo.smithy

restore() { git checkout -- "$model"; }
trap restore EXIT

if ! git diff --quiet -- "$model"; then
  echo "error: $model has local modifications; commit or stash them first" >&2
  exit 1
fi

echo "== stage 1: additive member (source-compatible) =="
sed -i.bak 's/^        done: Boolean$/        done: Boolean\n\n        notes: String/' "$model"
rm -f "$model.bak"
grep -q 'notes: String' "$model" || { echo "error: stage-1 edit did not apply" >&2; exit 1; }
"$bazel" test //... --verbose_failures

echo "== stage 2: new operation (compile error until handlers implement it) =="
sed -i.bak 's/    operations: \[AddTask, GetTask\]/    operations: [AddTask, GetTask, ListTasks]/' "$model"
rm -f "$model.bak"
grep -q 'ListTasks\]' "$model" || { echo "error: stage-2 edit did not apply" >&2; exit 1; }
cat >> "$model" <<'EOF'

@readonly
@http(method: "GET", uri: "/tasks")
operation ListTasks {
    output := {
        @required
        taskIds: TaskIdList
    }
}

list TaskIdList {
    member: String
}
EOF

if out=$("$bazel" build //:todo_integration_test 2>&1); then
  echo "error: expected the handler implementations to fail compilation" >&2
  exit 1
fi
if ! grep -q "ListTasks" <<< "$out"; then
  echo "error: build failed, but not on the new ListTasks handler method:" >&2
  echo "$out" >&2
  exit 1
fi

echo "OK: additive member rebuilt cleanly; new operation caught at compile time"
