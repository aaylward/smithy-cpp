#!/usr/bin/env bash
# The jsonRpc2 stream wire (ADR-0023, issue #123) as real processes through
# the module boundary: the GENERATED Tally service on the async surface,
# served by Beast in raw-text mode, driven by the GENERATED CLI client.
# Covers the opening envelope's initial-request member (the seed), running
# totals both ways, the clean end through the terminal result envelope, and
# the modeled Busted arriving typed through the terminal error envelope.
# Portable: no `timeout`, BSD-sed-safe.
set -euo pipefail

trap 'kill $(jobs -p) 2>/dev/null || true' EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

step() { echo "tally: $*" >&2; }
fail() {
  step "FAIL: $*"
  for f in "${TEST_TMPDIR}"/*.out "${log}"; do
    [ -f "$f" ] && { echo "--- $f" >&2; cat "$f" >&2; }
  done
  exit 1
}

server="$(pwd)/$1"
client="$(pwd)/$2"
log="${TEST_TMPDIR}/server.log"
cd "${TEST_TMPDIR}"

step "starting ${server}"
"$server" 0 2> "$log" &
server_pid=$!
port=""
for _ in $(seq 1 100); do
  port=$(sed -n 's/.*serving on :\([0-9][0-9]*\).*/\1/p' "$log" 2>/dev/null || true)
  [ -n "$port" ] && break
  sleep 0.2
done
[ -n "$port" ] || fail "server never reported its port"
step "serving on port ${port}"

step "a seeded session counts, then ends cleanly on the zero bump"
"$client" "$port" 10 5 7 0 > count.out || fail "counting client exited non-zero"
printf 'total 15\ntotal 22\nclosed\n' > count.expected
diff count.expected count.out || fail "count session output mismatch"

step "an unseeded session starts from zero"
"$client" "$port" 0 3 0 > zero.out || fail "unseeded client exited non-zero"
printf 'total 3\nclosed\n' > zero.expected
diff zero.expected zero.out || fail "unseeded session output mismatch"

step "counting below zero ends with the typed Busted"
if "$client" "$port" 1 -5 > busted.out; then
  fail "the busted session exited zero"
fi
grep -q '^error Busted: the tally went negative$' busted.out \
  || fail "missing the typed Busted terminal error"

step "SIGTERM: the server exits 0"
kill -TERM "$server_pid"
wait "$server_pid" || fail "server did not exit 0"
step "clean exit"
