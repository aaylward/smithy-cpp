#!/usr/bin/env bash
# The reconnect loop (ADR-0020, issue #116) as real processes through the
# module boundary: a GENERATED streaming server with SessionRegistry grace,
# GENERATED clients driven by shell commands. Covers the join broadcast, an
# abrupt kill -9 with a rejoin-within-grace receiving the roster snapshot
# (and no duplicate join announced), a /quit whose departure is announced
# only at grace expiry, a deliberate /leave announcing immediately, and the
# SIGTERM drain that never waits out ghosts. Portable: no `timeout`,
# BSD-sed-safe.
set -euo pipefail

# Whatever path ends this script, no server or client outlives it.
trap 'kill $(jobs -p) 2>/dev/null || true' EXIT

step() { echo "reconnect: $*" >&2; }
fail() {
  step "FAIL: $*"
  for f in "${TEST_TMPDIR}"/*.out "${log}"; do
    [ -f "$f" ] && { echo "--- $f" >&2; cat "$f" >&2; }
  done
  exit 1
}

wait_for_count() { # <file> <grep -E pattern> <count> <description>
  for _ in $(seq 1 150); do
    [ "$(grep -E -c "$2" "$1" 2>/dev/null || true)" -ge "$3" ] && return 0
    sleep 0.2
  done
  fail "timed out waiting for occurrence $3 of '$4' in $1"
}

wait_for() { # <file> <grep -E pattern> <description>
  wait_for_count "$1" "$2" 1 "$3"
}

server="$(pwd)/$1"
client="$(pwd)/$2"
log="${TEST_TMPDIR}/server.log"
cd "${TEST_TMPDIR}"

step "starting ${server}"
"$server" 0 3 2> "$log" &  # 3s grace: resumable in-test, expirable in-test
server_pid=$!
port=""
for _ in $(seq 1 100); do
  port=$(sed -n 's/.*serving on :\([0-9][0-9]*\).*/\1/p' "$log" 2>/dev/null || true)
  [ -n "$port" ] && break
  sleep 0.2
done
[ -n "$port" ] || fail "server never reported its port"
step "serving on port ${port}"

step "ada and bob join"
mkfifo ada.in bob.in bob2.in
"$client" "$port" ada < ada.in > ada.out &
ada_pid=$!
exec 3> ada.in
wait_for ada.out '^note joined:ada$' "ada's own join broadcast"
"$client" "$port" bob < bob.in > bob.out &
bob_pid=$!
exec 4> bob.in
wait_for bob.out '^note joined:bob$' "bob's own join broadcast"
wait_for ada.out '^note joined:bob$' "bob's arrival at ada"
echo "hello" >&3
wait_for bob.out '^note ada:hello$' "ada's note at bob"

step "kill -9, then a rejoin within grace: resume with the roster snapshot"
kill -9 "$bob_pid" 2>/dev/null || true
wait "$bob_pid" 2>/dev/null || true
exec 4>&-
"$client" "$port" bob < bob2.in > bob2.out &
bob2_pid=$!
exec 4> bob2.in
wait_for bob2.out '^note snapshot:ada,bob$' "the roster snapshot at the resumed bob"
echo "back" >&4
wait_for ada.out '^note bob:back$' "the resumed session speaks"
[ "$(grep -c '^note joined:bob$' ada.out)" -eq 1 ] || fail "resume must not re-announce the join"

step "a /quit detaches; grace expiry announces the departure"
echo "/quit" >&4
exec 4>&-
wait "$bob2_pid" || fail "quitting client exited non-zero"
# Nothing announced yet: the session sits detached until the 3s grace
# runs out and on_expired finally tells the room.
wait_for ada.out '^note left:bob$' "the deferred departure at expiry"

step "a deliberate /leave announces immediately"
echo "/leave" >&3
wait_for ada.out '^note left:ada$' "ada's own goodbye"
wait_for ada.out '^closed$' "ada's clean close"
exec 3>&-
wait "$ada_pid" || fail "ada's client exited non-zero"

step "SIGTERM: the hub drains without waiting out ghosts, exit 0"
kill -TERM "$server_pid"
wait "$server_pid" || fail "server did not exit 0 after draining"
grep -q "draining" "$log" || fail "server log missing the drain checkpoint"
grep -q "chat-hub: drained" "$log" || fail "server log missing the drained checkpoint"
step "clean exit"
