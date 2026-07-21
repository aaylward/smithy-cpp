#!/usr/bin/env bash
# The jsonRpc2 stream wire (ADR-0023, issue #123) as real processes through
# the module boundary: the GENERATED Tally service on the async surface,
# served by Beast in raw-text mode, driven by the GENERATED CLI client —
# and by the raw peer, which types whatever a browser could at the socket.
# Covers the opening envelope's initial-request member (the seed), running
# totals both ways, the clean end through the terminal result envelope, the
# modeled Busted arriving typed through the terminal error envelope, and
# the policing edges the well-behaved client cannot produce: the -32601
# unknown-method refusal and the -32700 mid-stream violation terminal,
# byte-pinned, each followed by the close and nothing else.
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
raw_peer="$(pwd)/$3"
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

step "an unknown method earns the pinned -32601 terminal, then the close"
printf '%s\n' \
  '{"jsonrpc":"2.0","method":"Nope","params":{},"id":4}' \
  | "$raw_peer" "$port" > unknown.out || fail "raw peer (unknown method) exited non-zero"
printf '%s\n' \
  'recv {"error":{"code":-32601,"data":{"__type":"UnknownOperationException"},"message":"unknown method: Nope"},"id":4,"jsonrpc":"2.0"}' \
  'closed' > unknown.expected
diff unknown.expected unknown.out || fail "unknown-method output mismatch"

step "mid-stream garbage on a live session earns the pinned -32700 terminal"
# The total before the violation proves the session was live; exactly one
# envelope follows it, then the close — never a success terminal.
printf '%s\n' \
  '{"jsonrpc":"2.0","method":"Count","params":{"start":5},"id":9}' \
  '{"jsonrpc":"2.0","method":"bump","params":{"id":9,"payload":{"by":2}}}' \
  'not json' \
  | "$raw_peer" "$port" > garbage.out || fail "raw peer (mid-stream garbage) exited non-zero"
printf '%s\n' \
  'recv {"jsonrpc":"2.0","method":"total","params":{"id":9,"payload":{"value":7}}}' \
  'recv {"error":{"code":-32700,"data":{"__type":"SerializationException"},"message":"text frame is not JSON"},"id":9,"jsonrpc":"2.0"}' \
  'closed' > garbage.expected
diff garbage.expected garbage.out || fail "mid-stream violation output mismatch"

step "SIGTERM: the server exits 0"
kill -TERM "$server_pid"
wait "$server_pid" || fail "server did not exit 0"
step "clean exit"
