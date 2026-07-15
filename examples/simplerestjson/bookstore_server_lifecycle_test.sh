#!/usr/bin/env bash
# The lifecycle the example exists to demonstrate, executed: start on an
# ephemeral port, serve a POST and a GET, exercise the modeled 404, SIGTERM,
# assert a clean (drained) exit. Portable across the CI matrix: no `timeout`,
# BSD-sed-safe. Each phase logs a checkpoint so a failure names its step.
set -euo pipefail

step() { echo "lifecycle: $*" >&2; }

binary="$1"
log="${TEST_TMPDIR}/server.log"

step "starting ${binary}"
"$binary" 0 2> "$log" &
server_pid=$!

port=""
for _ in $(seq 1 100); do
  port=$(sed -n 's/.*serving on :\([0-9][0-9]*\).*/\1/p' "$log")
  [ -n "$port" ] && break
  sleep 0.2
done
if [ -z "$port" ]; then
  step "server never reported its port; log:"
  cat "$log" >&2
  exit 1
fi
step "serving on port ${port}"

step "POST /books"
curl -sfS -X POST "localhost:${port}/books" \
  -H 'Content-Type: application/json' \
  -d '{"isbn":"0-306-40615-2","title":"Petriflora"}' > /dev/null

step "GET /books/0-306-40615-2"
curl -sfS "localhost:${port}/books/0-306-40615-2" | grep -q '"title":"Petriflora"'

step "GET /books/none (modeled 404: x-error-type header + typed body)"
notfound_status=$(curl -sS -D "${TEST_TMPDIR}/notfound.headers" \
  -o "${TEST_TMPDIR}/notfound.json" -w '%{http_code}' "localhost:${port}/books/none")
[ "$notfound_status" = "404" ]
grep -i -q '^x-error-type: *BookNotFound' "${TEST_TMPDIR}/notfound.headers"
grep -q 'no book: none' "${TEST_TMPDIR}/notfound.json"

step "SIGTERM: expecting drained exit 0"
kill -TERM "$server_pid"
wait "$server_pid"  # non-zero (or a signal death) fails the test: no clean drain
grep -q "draining" "$log"
step "clean exit"
