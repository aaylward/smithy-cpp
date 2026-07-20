#!/usr/bin/env bash
# The consumer-hub example executed as real processes (issue #112): one
# hub_server, several hub_clients over real WebSockets, the whole
# conversation driven by shell commands through the clients' stdin. Covers
# fan-out with per-viewer redaction, a watcher, the typed nickname refusal,
# a clean leave, an abrupt quit with nickname reuse, live occupancy on the
# unary neighbor, and the SIGTERM → drain → clean-exit lifecycle. Portable
# across the CI matrix like bookstore_server_lifecycle_test.sh: no
# `timeout`, BSD-sed-safe. Each phase logs a checkpoint so a failure names
# its step.
set -euo pipefail

step() { echo "hub: $*" >&2; }
fail() {
  step "FAIL: $*"
  for f in "${TEST_TMPDIR}"/*.out "${log}"; do
    [ -f "$f" ] && { echo "--- $f" >&2; cat "$f" >&2; }
  done
  exit 1
}

# Polls until the file contains the Nth line matching the pattern — how the
# test waits out event-stream delivery without racing it. Counted matches
# exist for events whose first occurrence is already in the file (a second
# departure by the same nickname).
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

# Absolute paths: the fifos live in TEST_TMPDIR, so the script cd's away
# from the runfiles root the $(rootpath) args are relative to.
server="$(pwd)/$1"
client="$(pwd)/$2"
log="${TEST_TMPDIR}/server.log"
cd "${TEST_TMPDIR}"

step "starting ${server}"
"$server" 0 2> "$log" &
server_pid=$!
port=""
for _ in $(seq 1 100); do
  port=$(sed -n 's/.*serving on :\([0-9][0-9]*\).*/\1/p' "$log")
  [ -n "$port" ] && break
  sleep 0.2
done
[ -n "$port" ] || fail "server never reported its port"
step "serving on port ${port}"

step "ada and grace join the lobby"
mkfifo ada.in grace.in
"$client" "$port" lobby ada < ada.in > ada.out &
ada_pid=$!
exec 3> ada.in
wait_for ada.out '^joined ada$' "ada's own join echo"
"$client" "$port" lobby grace < grace.in > grace.out &
grace_pid=$!
exec 4> grace.in
wait_for grace.out '^joined grace$' "grace's own join echo"
wait_for ada.out '^joined grace$' "grace's arrival fanned out to ada"

step "one message, two renderings (per-viewer redaction)"
echo "hello everyone" >&3
wait_for grace.out '^message ada hello everyone$' "ada's message at grace"
wait_for ada.out '^message you hello everyone$' "ada's own echo says you"
echo "hi ada" >&4
wait_for ada.out '^message grace hi ada$' "grace's reply at ada"
wait_for grace.out '^message you hi ada$' "grace's own echo says you"

step "a watcher observes the room without sending"
"$client" "$port" lobby --watch < /dev/null > watch.out &
watch_pid=$!
# The watcher registers a beat after its dial returns, so prove it by
# traffic: repeat ada's probe until it shows up (the duplicates are inert
# noise for every other assertion).
observed=0
for _ in $(seq 1 100); do
  echo "for the record" >&3
  sleep 0.2
  if grep -E -q '^message ada for the record$' watch.out; then
    observed=1
    break
  fi
done
[ "$observed" = 1 ] || fail "the watcher never observed lobby traffic"

step "a nickname collision is refused with the typed error"
"$client" "$port" lobby ada < /dev/null > impostor.out || true
wait_for impostor.out '^error Kicked: .*already in lobby' "the typed refusal"

step "unary neighbor reports live occupancy (watchers excluded)"
rooms_body=$(curl -sfS "localhost:${port}/rooms")
echo "$rooms_body" | grep -q '"name":"lobby"' || fail "no lobby in: $rooms_body"
echo "$rooms_body" | grep -q '"members":2' \
  || fail "expected lobby occupancy 2 (ada + grace, watcher excluded), got: $rooms_body"

step "grace leaves cleanly"
echo "/leave" >&4
wait_for grace.out '^left grace$' "grace's goodbye"
wait_for grace.out '^closed$' "grace's clean close"
wait "$grace_pid" || fail "grace's client exited non-zero"
exec 4>&-
wait_for ada.out '^left grace$' "grace's departure at ada"

step "abrupt quit frees the nickname"
mkfifo grace2.in
"$client" "$port" lobby grace < grace2.in > grace2.out &
grace2_pid=$!
exec 4> grace2.in
wait_for grace2.out '^joined grace$' "grace rejoined after leaving"
echo "/quit" >&4
wait_for_count ada.out '^left grace$' 2 "the vanish still announced"
exec 4>&-
wait "$grace2_pid" || fail "quitting client exited non-zero"

step "SIGTERM: the hub drains, clients see clean closes, exit 0 all around"
kill -TERM "$server_pid"
wait_for ada.out '^closed$' "ada's drain close"
wait_for watch.out '^closed$' "the watcher's drain close"
exec 3>&-
wait "$ada_pid" || fail "ada's client exited non-zero"
wait "$watch_pid" || fail "the watcher exited non-zero"
wait "$server_pid" || fail "server did not exit 0 after draining"
grep -q "draining" "$log" || fail "server log missing the drain checkpoint"
grep -q "chat-hub: drained" "$log" || fail "server log missing the drained checkpoint"
step "clean exit"
