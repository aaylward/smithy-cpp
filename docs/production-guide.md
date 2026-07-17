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
retry loop below. A `BeastHttpClient` built via `FromConfig` (see
[Production client transport](#production-client-transport)) honors the
same value, so the timeout is configured once. If you inject a transport
you constructed yourself, that transport owns timeout enforcement; the
built-in behavior is the reference.

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

## Auth

Services modeled with `@httpBearerAuth` or `@httpApiKeyAuth` get credential
wiring generated into their clients — set the provider on the config and
every request carries it (providers are called per request, so rotation
just works):

```cpp
config.bearer_token = [] { return LoadToken(); };   // @httpBearerAuth
config.api_key = [] { return LoadApiKey(); };       // @httpApiKeyAuth
```

Bearer tokens ride as `authorization: Bearer <token>`; API keys go where
the model binds them — a named header (with the trait's scheme prefix, if
any) or a query parameter. A null provider leaves requests anonymous.

Server-side, the matching guards ship as middleware
(`smithy/server/middleware.h`):

```cpp
transport.Start(smithy::server::Chain(
    {smithy::server::RequireBearerAuth([](const std::string& token) {
      return TokenIsValid(token);  // 401 otherwise
    })},
    server.Handler()));
// Or: smithy::server::RequireApiKeyHeader("x-api-key", /*scheme=*/"", validator)
```

Vendor-specific signing schemes (e.g. SigV4) are out of scope by design;
implement them as an `Interceptor` (below).

## Pagination

Operations modeled with `@paginated` (top-level string tokens) get a
generated paginator: `client.PaginateListCities(input)` returns a
`ListCitiesPaginator` that is a single-pass range (issue #49) — iteration
yields one `smithy::Outcome<Page>&` per page, and a failed call is yielded
exactly once before the range ends by itself, so the loop needs no manual
token or nullopt protocol:

```cpp
for (auto& page : client.PaginateListCities({.pageSize = 100})) {
  if (!page.ok()) return page.error();   // pagination stops on first error
  for (const auto& city : page->items) Process(city);
}
```

The paginator owns a copy of the client and input, so it outlives both; the
range is single-pass (call `begin()` once — range-for does). The pull API
remains for manual control: `Next()` yields one page at a time,
`std::nullopt` once the service stops returning a next token, or the first
failed call's error. An empty-string token is treated as end-of-pagination
(defensive: it can never loop forever on a server echoing empty tokens).

## Client interceptors

`config.interceptors` (`smithy/client/interceptor.h`) hooks user code around
every HTTP attempt a generated client makes — auth headers, tracing ids,
request/response logging — without touching generated code:

```cpp
class BearerAuth final : public smithy::Interceptor {
 public:
  void ModifyBeforeTransmit(smithy::http::HttpRequest& request, int attempt) override {
    request.headers.Set("authorization", "Bearer " + LoadToken());
  }
  void ReadAfterTransmit(const smithy::http::HttpRequest& request,
                         const smithy::Outcome<smithy::http::HttpResponse>& outcome,
                         int attempt) override {
    LogAttempt(request.target, attempt, outcome.ok() ? outcome->status : -1);
  }
};

config.interceptors.push_back(std::make_shared<BearerAuth>());
```

Interceptors run in registration order, around each attempt (retries included
— `attempt` is 1-based). `ModifyBeforeTransmit` mutates a fresh copy of the
request per attempt, so edits never accumulate across retries or leak into
the caller's view. Hooks must not throw.

## Server middleware

Generated servers expose their router as a plain
`smithy::http::RequestHandler`, so cross-cutting server behavior — auth
checks, request logging, metrics — composes as middleware outside the
generated code (`smithy/server/middleware.h`), with any transport:

```cpp
WeatherServer server(handler);

// Policy stays an application dependency (your rate limiter, your metrics
// backend); the middleware owns only the composition point.
auto limiter = std::make_shared<MyRateLimiter>(/* window, budget */);
auto db = std::make_shared<MyDbPool>(/* ... */);

transport.Start(smithy::server::Chain(
    {// Outermost: shed abusive traffic before it costs anything. Trust
     // x-forwarded-for only behind a proxy that sets it.
     smithy::server::Guard(
         [limiter](const smithy::http::HttpRequest& request) {
           return limiter->Allow(
               request.headers.Get("x-forwarded-for").value_or(""));
         },
         smithy::server::TooManyRequests(std::chrono::seconds(30))),
     // Observe everything admitted — health probes included. on_start
     // (optional) enables an in-flight gauge; on_complete carries
     // method/target/operation/status/duration/trace_parent.
     smithy::server::Observe(
         [](const smithy::server::RequestObservation& o) {
           // gauge -1; count 1; latency o.duration — feed any backend.
         },
         [](const smithy::server::RequestStart& s) {
           // gauge +1 (labeled by s.method/s.target; the operation is not
           // known until the router runs).
         }),
     // Liveness: GET or HEAD /livez -> 200 {"status":"healthy"} (no body
     // for HEAD); everything else passes through to the router.
     smithy::server::HealthEndpoint("/livez"),
     // Readiness: the same endpoint with checks. Every probe runs on every
     // request (no caching — a cached 200 would hide a dependency outage);
     // any failure answers 503 {"status":"unhealthy","failing":["db"]}.
     // A throwing probe counts as failing, never unwinds into the transport.
     smithy::server::HealthEndpoint(
         "/readyz", {{"db", [db] { return db->Alive(); }}})},
    server.Handler()));
```

The first middleware in the chain is outermost: it sees the request first and
can short-circuit before anything below it runs (so `Guard`'s rejections never
reach `Observe` — track rejection rates in the limiter itself). Health
probes typically arrive without `x-forwarded-for`, so under this order they
share the empty-string key with any client hitting the service directly, and
one such direct-connect client can exhaust that key's budget and starve
probes into liveness or readiness failures; where that risk is real, compose
the `HealthEndpoint` instances outside `Guard`, or have `admit` always accept
the empty key. Readiness probes run on the transport's request thread, once
per probe request — keep them cheap (a pool's cached connectivity flag, not a
fresh dial) and thread-safe. `Guard` is the generic admission primitive — rate limiting (above), IP
allowlists, maintenance mode — admit/reject callbacks in, one decision point
out. `Observe`'s callbacks run on the transport's request thread (keep them
cheap or hand off) and always pair: when dispatch throws, `on_complete`
reports a 500 completion before the exception reaches the transport's
containment, so an in-flight gauge can never leak. Throwing callbacks are
logged and swallowed.

## Serving lifecycle

The pattern for a long-running server is SIGTERM/SIGINT → `Start`/block/`Stop`, with the drain
([Server hardening](#server-hardening) has the contract) doing the graceful half. This is
`main()` from [`examples/simplerestjson/serve_main.cc`](../examples/simplerestjson/serve_main.cc)
verbatim — compiled, lifecycle-tested in CI, and runnable as
`bazel run //examples/simplerestjson:bookstore_server`:

```cpp
int main(int argc, char** argv) {
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  // Before Start(): threads the transport creates inherit this mask, so the
  // shutdown signals reach only the sigwait() below.
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

  BookstoreServer server(std::make_shared<InMemoryBookstore>());
  smithy::http::BeastServerTransport transport({
      .address = "0.0.0.0",
      .port = argc > 1 ? std::atoi(argv[1]) : 8080,  // 0 binds an ephemeral port
      .drain_timeout_seconds = 10,
  });
  smithy::Outcome<smithy::Unit> started = transport.Start(server.Handler());
  if (!started.ok()) {
    std::fprintf(stderr, "bookstore: start failed: %s\n", started.error().message().c_str());
    return 1;
  }
  std::fprintf(stderr, "bookstore: serving on :%d (SIGTERM or Ctrl-C drains and exits)\n",
               transport.port());

  int signal_number = 0;
  sigwait(&shutdown_signals, &signal_number);  // serve until SIGTERM/SIGINT
  std::fprintf(stderr, "bookstore: signal %d, draining\n", signal_number);
  transport.Stop();  // in-flight requests get drain_timeout_seconds to finish
  return 0;
}
```

Under Kubernetes: SIGTERM is exactly what the kubelet sends, so size
`terminationGracePeriodSeconds` above `drain_timeout_seconds`, and compose the `/livez` +
`/readyz` probes from the middleware chain above in front of the handler — readiness flips
traffic away while the drain finishes.

## Observability

The runtime's observability story is deliberately SDK-free: enriched hooks
on both sides plus W3C Trace Context helpers, so any backend — including
OpenTelemetry — plugs in without the core taking a telemetry dependency.

**Server:** `Observe` (above) reports, per request: `method`, `target`,
`operation` (the Smithy operation that handled it, stamped by the generated
router; empty for 404/405/400 dispatch failures), `status`, `duration`, and
`trace_parent` — the incoming W3C `traceparent` header, verbatim, for log
correlation. An optional `on_start` callback fires before dispatch (method and
target only), enabling in-flight gauges; start/complete always pair, even when
the handler throws.

**Client:** two ready-made interceptors in
`smithy/client/observability.h`:

```cpp
// Metrics/logging: one callback per HTTP attempt (retries visible).
config.interceptors.push_back(smithy::ObserveAttempts(
    [](const smithy::AttemptObservation& a) {
      // a.method, a.target, a.attempt, a.status (-1 = transport error),
      // a.error_message
    }));

// Distributed tracing: sets a W3C traceparent header on every attempt that
// lacks one. Pass a callback returning your application's active trace
// context to join an existing trace; omit it to start fresh roots.
config.interceptors.push_back(smithy::PropagateTraceContext());
```

`smithy/http/trace_context.h` has the underlying helpers —
`ParseTraceparent`, `FormatTraceparent`, `GenerateTraceContext`,
`GenerateSpanId` — for building richer integrations (e.g. a server
middleware that opens a span from `RequestObservation::trace_parent`).

**OpenTelemetry:** not bundled, by design — opentelemetry-cpp's dependency
tree (protobuf, gRPC for OTLP) would violate the runtime's dep-light rule.
The hooks above map 1:1 onto OTel spans and metrics; an optional
`//runtime:otel` adapter is planned post-0.1.0 once the hook shapes have
survived production use (see PLAN.md).

## Production client transport

`SocketHttpClient` (the `config.endpoint` default) is plaintext,
connection-per-request — fine for tests and simple internal deployments.
Production clients should inject `BeastHttpClient` (ADR-0007): keep-alive
connection pooling, per-request timeouts, and TLS with certificate and
hostname verification on by default.

Every knob lives on the one `ClientConfig` (issue #49):
`config.tls.ca_pem` / `config.tls.verify_peer` for trust,
`config.max_idle_connections` for pooling, and the same
`config.request_timeout_ms` the rest of this guide tunes.
`BeastHttpClient::FromConfig` reads them all — endpoint, TLS, timeout, and
pool size come from the config, so nothing is configured twice:

```cpp
smithy::ClientConfig config;
config.endpoint = "https://api.example.com";   // identity + path prefix
config.tls.ca_pem = corp_ca_pem;               // only when not publicly trusted
auto transport = smithy::http::BeastHttpClient::FromConfig(config);
if (!transport) { /* bad URL */ }
config.http_client = *transport;               // the wire
auto client = MyServiceClient::Create(std::move(config));
```

`ca_pem` (PEM text, not a file path) replaces the system trust roots for
private CAs; `config.tls.verify_peer = false` exists as an escape hatch for
local experiments and must never reach production. The lower-level
`BeastHttpClient::Options` constructor remains for tests and custom wiring
(loopback ports, deliberately broken TLS). `//runtime:http_beast` is
self-contained — it carries the asio SSL implementation and the BoringSSL
dependency itself, so no extra build flags are needed.

## Server hardening

The production server transport (`BeastServerTransport`, ADR-0006) enforces
per-connection timeouts (`request_timeout_seconds`) and body- and header-size
limits (`max_body_bytes`, `max_header_bytes`) — over-limit requests are
answered with `413 Content Too Large` / `431 Request Header Fields Too Large`
and `Connection: close`, followed by a bounded lingering close (a few seconds
/ 256 KiB of drain) so the status stays readable; a client that streams past
the budget without reading may still see a reset, which is inherent to the
recipe.

Concurrent connections are capped by `max_connections` (default 1024; 0
disables the cap): at the cap the server pauses accepting and new
connections wait in the kernel's listen backlog until a session closes, so
a connection flood cannot exhaust file descriptors or memory. Idle
keep-alive sessions still expire on `request_timeout_seconds`, so they
cannot pin the cap.

Handlers execute on their own pool (`handler_threads`, default 16; 0 runs
them inline on the io threads): a handler that blocks on a database or
downstream call cannot starve the `threads` io threads that accept
connections and read and write the wire, so already-computed responses keep
flowing even while every handler is blocked. Size `handler_threads` for
your handlers' blocking profile; handler implementations must be
thread-safe either way, as concurrent requests dispatch concurrently.

Responses are framed by the transport alone: any `content-length`,
`transfer-encoding`, or `connection` header set by a handler is dropped
rather than emitted beside the transport's own (a duplicate or conflicting
framing pair is the classic request-smuggling vector); both server
transports enforce this.

The transport terminates TLS when
`tls_certificate_chain_pem` + `tls_private_key_pem` are set (ADR-0007), and
drains on `Stop()`: new
connections and keep-alive reads cease immediately, while requests already
read off the wire get up to `drain_timeout_seconds` (default 10) to finish
writing their responses before the thread pool is torn down. `Stop()` is
itself bounded: a handler that never returns cannot wedge shutdown — past
the drain deadline plus a short grace, the stuck worker is abandoned (its
thread and the transport's internal state deliberately leak, since a thread
cannot be killed safely) and `Stop()` returns; if the handler ever does
return, the abandoned reaper finishes the cleanup in the background. See
[server-guide.md](server-guide.md).
