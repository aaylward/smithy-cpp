#!/usr/bin/env bash
# The lifecycle the example exists to demonstrate, executed: start on an
# ephemeral port, serve a POST and a GET, SIGTERM, assert a clean (drained)
# exit. Portable across the CI matrix: no `timeout`, BSD-sed-safe.
set -euo pipefail

binary="$1"
log="${TEST_TMPDIR}/server.log"

"$binary" 0 2> "$log" &
server_pid=$!

port=""
for _ in $(seq 1 100); do
  port=$(sed -n 's/.*serving on :\([0-9][0-9]*\).*/\1/p' "$log")
  [ -n "$port" ] && break
  sleep 0.2
done
if [ -z "$port" ]; then
  echo "server never reported its port; log:" >&2
  cat "$log" >&2
  exit 1
fi

curl -sfS -X POST "localhost:${port}/books" \
  -H 'Content-Type: application/json' \
  -d '{"isbn":"0-306-40615-2","title":"Petriflora"}' > /dev/null
curl -sfS "localhost:${port}/books/0-306-40615-2" | grep -q '"title":"Petriflora"'
curl -sS "localhost:${port}/books/none" | grep -q 'BookNotFound'

kill -TERM "$server_pid"
wait "$server_pid"  # non-zero (or a signal death) fails the test: no clean drain
grep -q "draining" "$log"
