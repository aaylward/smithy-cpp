# Production guide

How to configure generated smithy-cpp clients and servers for production use:
timeouts, retries, and request compression. Every knob lives on
`smithy::ClientConfig` (`smithy/client/config.h`), so the guidance below
applies to every generated client the same way.

```cpp
#include "myservice/client.h"
#include "smithy/client/config.h"

smithy::ClientConfig config;
config.endpoint = "http://api.example.com:8080";
config.request_timeout_ms = 5000;
config.retry.max_attempts = 5;
auto client = myservice::MyServiceClient::Create(std::move(config));
```

## Timeouts

`config.request_timeout_ms` (default 30000) bounds each HTTP attempt —
connect plus request plus response — on the built-in socket transport. A
timed-out attempt fails with a retryable transport error, so it feeds the
retry loop below. If you inject your own `http_client`, the transport owns
timeout enforcement; the built-in behavior is the reference.

Pick a timeout from your service's latency tail (a small multiple of p99),
not a comfortable-sounding round number: with retries enabled the worst-case
caller wait is roughly `max_attempts × timeout` plus backoff sleeps.

## Retries

Every generated client sends through `smithy::SendWithRetries`
(`smithy/client/retry.h`). Two failure classes are retried:

- **Transport errors flagged retryable** — connection refused/reset,
  timeouts.
- **Transient HTTP statuses** — 429, 500, 502, 503, 504 (the set every
  Smithy SDK treats as transient). Other statuses, including 400/403/404 and
  modeled errors, are returned immediately.

Backoff is **full-jitter exponential**: retry *n* sleeps
`uniform(0, min(max_backoff, initial_backoff × 2^(n-1)))`. Jitter
desynchronizes clients after a shared failure, so a recovering server is not
hit by a synchronized thundering herd.

```cpp
config.retry.max_attempts = 3;                              // total tries; 1 disables retries
config.retry.initial_backoff = std::chrono::milliseconds(100);
config.retry.max_backoff = std::chrono::milliseconds(20000);
```

Guidance:

- **Interactive paths:** keep `max_attempts` low (2–3) and cap
  `max_backoff` near your latency budget; a user-facing call gains nothing
  from a 20-second sleep.
- **Batch/background paths:** raise `max_attempts` and let `max_backoff`
  breathe; throttling (429) resolves on its own if you back off.
- **Idempotency:** retries resend the same serialized request.
  `@idempotencyToken` members are generated once per call and reused across
  attempts, so the server can deduplicate. For non-idempotent operations
  without a token, weigh whether a retried timeout can double-apply.
- **Tests:** wire-exact tests set `config.retry.max_attempts = 1` (generated
  suites already do). To test retry behavior deterministically, inject
  `config.retry.sleep` and `config.retry.jitter`.

## Request compression

Operations modeled with `@requestCompression(encodings: ["gzip"])` gzip
their request body when it reaches
`config.request_min_compression_size_bytes` (default 10240, the Smithy
default; 0 compresses everything). The client appends `gzip` to any existing
`Content-Encoding` header value. Nothing is configured per call — model the
trait and the generated client and server both handle it:

- **Client:** compresses via `smithy::GzipCompress` (`//runtime:compression`,
  zlib) after serialization, before send.
- **Server:** generated routes for `@requestCompression` operations
  transparently gunzip requests arriving with `Content-Encoding: gzip`
  (or `..., gzip`) and reject malformed gzip bodies with a 400
  serialization error. Decompression is capped (64 MB) to stop
  decompression-bomb inputs.

Compression trades CPU for bytes: leave the 10 KiB threshold alone unless
you have measured small-payload wins; compressing tiny bodies usually
inflates them.

## Server hardening

The production server transport (`BeastServerTransport`, ADR-0006) already
enforces per-connection timeouts, body-size limits, and graceful shutdown;
see [server-guide.md](server-guide.md). Phase 7b extends this area
(thread-pool sizing, drain, slow-client handling, logging/metrics hooks) —
see [PLAN.md](PLAN.md).
