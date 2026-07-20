#!/usr/bin/env bash
# The reconnect loop (ADR-0020, issue #116) as real processes through the
# module boundary: a GENERATED streaming server with SessionRegistry grace,
# GENERATED clients driven by shell commands. Covers the join broadcast, an
# abrupt kill -9 with a rejoin-within-grace receiving the roster snapshot
# (and no duplicate join announced), a /quit whose departure is announced
# only AFTER the grace deadline (deferral asserted, then expiry), a
# deliberate /leave announcing immediately, and a SIGTERM drain holding a
# fresh long-grace ghost that must be expired, not waited out. Portable:
# no `timeout`, BSD-sed-safe.
set -euo pipefail

# Whatever path ends this script, no server or client outlives it. An
# EXIT trap alone misses untrapped signals (bash skips it when killed),
# so route the sandbox's TERM/INT through exit.
trap 'kill $(jobs -p) 2>/dev/null || true' EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

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
# The deferral itself, pinned: one second in (safely inside the 3s
# grace) nothing has been announced — a regression to the immediate
# Remove+announce path fails here, not just "announces sooner".
sleep 1
[ "$(grep -c '^note left:bob$' ada.out || true)" -eq 0 ] \
  || fail "the departure was announced before grace expired"
wait_for ada.out '^note left:bob$' "the deferred departure at expiry"

step "a deliberate /leave announces immediately"
echo "/leave" >&3
wait_for ada.out '^note left:ada$' "ada's own goodbye"
wait_for ada.out '^closed$' "ada's clean close"
exec 3>&-
wait "$ada_pid" || fail "ada's client exited non-zero"

step "SIGTERM: the hub drains, exit 0"
kill -TERM "$server_pid"
wait "$server_pid" || fail "server did not exit 0 after draining"
grep -q "draining" "$log" || fail "server log missing the drain checkpoint"
grep -q "chat-hub: drained" "$log" || fail "server log missing the drained checkpoint"

step "a long-grace ghost never stalls the drain"
# A fresh hub whose 300s grace dwarfs its 5s drain timeout: a ghost made
# moments before SIGTERM must be expired by the drain, not waited out —
# a regression there exits 1 on the drain timeout, a hard failure.
log2="${TEST_TMPDIR}/server2.log"
"$server" 0 300 2> "$log2" &
server2_pid=$!
port2=""
for _ in $(seq 1 100); do
  port2=$(sed -n 's/.*serving on :\([0-9][0-9]*\).*/\1/p' "$log2" 2>/dev/null || true)
  [ -n "$port2" ] && break
  sleep 0.2
done
[ -n "$port2" ] || fail "second server never reported its port"
mkfifo eve.in
"$client" "$port2" eve < eve.in > eve.out &
eve_pid=$!
exec 5> eve.in
wait_for eve.out '^note joined:eve$' "eve's join at the second hub"
kill -9 "$eve_pid" 2>/dev/null || true
wait "$eve_pid" 2>/dev/null || true
exec 5>&-
sleep 1  # the hub notices the dead wire and parks eve detached
kill -TERM "$server2_pid"
wait "$server2_pid" || fail "drain waited out the ghost instead of expiring it"
grep -q "chat-hub: drained" "$log2" || fail "second server log missing the drained checkpoint"
step "clean exit"
