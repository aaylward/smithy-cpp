# Server guide: implementing a generated service

Phase 4 generates a server counterpart for every service: `server.h`/`src/server.cc` with a
`<Service>Handler` interface and a `<Service>Server`, exposed as the module's `:server` Bazel
target.

## Implementing a handler

Handler implementations must be **thread-safe**: the production transport
(`BeastServerTransport`, ADR-0006) dispatches requests on a thread pool, so any mix of
operations can run concurrently against the one handler instance you pass to the server.
Guard shared state (the quickstart's in-memory handler shows the minimal mutex pattern).

```cpp
#include "example/weather/server.h"

class MyHandler final : public example::weather::WeatherHandler {
 public:
  smithy::Outcome<GetCityOutput> GetCity(const GetCityInput& input,
                                         const smithy::server::RequestContext& context) override {
    if (/* not found */) {
      smithy::Error error = smithy::Error::Modeled("NoSuchResource", "no city: " + input.cityId);
      error.set_detail(NoSuchResource{.resourceType = "City"});  // serializes the typed body
      return error;
    }
    std::clog << "GetCity from " << context.request->peer_address << "\n";  // or leave it unnamed
    return GetCityOutput{...};
  }
  // ... one method per operation
};
```

- **Request metadata**: the second parameter carries what the typed input doesn't model
  (issue #46). `context.request` is the raw `smithy::http::HttpRequest`: read unmodeled
  headers (`context.request->headers.Get("x-tenant")`), the transport-stamped client
  address (`context.request->peer_address`, `"ip:port"`, empty on the in-memory Loopback),
  or the request's `traceparent` — always present and parseable behind a transport, since
  the ingress mints a root context when the client sent none (ADR-0011); parse it with
  `smithy::http::ParseTraceparent` and `GenerateSpanId` (`smithy/http/trace_context.h`) to
  open child spans. `context.labels`
  and `context.query_params` hold the decoded routing captures. Handlers that need none of
  it leave the parameter unnamed. Behind a reverse proxy, derive the real client with
  `smithy::http::ClientAddress` over a `TrustedProxies` set (`smithy/http/forwarded.h`,
  ADR-0012) instead of reading `x-forwarded-for` yourself — the raw header is
  client-authored.

- **Modeled errors**: return `smithy::Error::Modeled("<ErrorShapeName>", message)`. The server
  maps the code to the shape's `@httpError` status (else 400/`@error("server")` → 500) and the
  protocol's error identity — simpleRestJson sets the neutral `X-Error-Type` header and serializes the
  detail as the body; rpcv2Cbor writes the fully qualified shape id as `__type` in the CBOR
  body; jsonRpc2 answers a JSON-RPC error object whose `code` is the `@httpError` status and
  whose `data` carries the members plus `__type`. Attach the typed structure with `set_detail()` to serialize its members (simpleRestJson
  `@httpHeader`-bound error members become response headers); the generic message is added
  only when the detail carries no message member of its own.
- **Validation/serialization errors** (including malformed request input the framework catches
  before your handler runs) map to 400; any other failure is a non-leaking 500
  `InternalFailure` carrying the request's trace id as `x-correlation-id` (ADR-0011) — a
  returned error correlates exactly like a thrown one.
- **A handler that throws** (rather than returning an `Error`) is contained by the transport
  layer: the exception is converted into a 500 whose `x-correlation-id` header is the
  request's trace id (ADR-0011), and the same id plus the exception's `what()` is written to
  `std::clog`. One throwing request fails alone — it never unwinds into the transport's I/O
  thread and terminates the process. Prefer returning `smithy::Error` for expected failures;
  the catch-all is a safety net, not a control path.

## Running a server

The server is transport-agnostic: `Handler()` returns a `smithy::http::RequestHandler` that
plugs into any `HttpServerTransport`:

```cpp
example::weather::WeatherServer server(std::make_shared<MyHandler>());

smithy::http::BeastServerTransport transport(options);  // production (ADR-0006)
transport.Start(server.Handler());
// or smithy::http::Loopback for in-process tests. (SocketHttpServer is test-only: one
// connection at a time, loopback only — its Start() says so on std::clog.)
```

Cross-cutting behavior (auth checks, request logging, metrics) wraps `server.Handler()` as
user-supplied middleware — `smithy::server::Chain` composes it outside the generated router,
and `smithy::server::Observe` is the built-in logging/metrics hook. See
[production-guide.md](production-guide.md).

Routing (method + URI pattern from `@http`, greedy labels, 404/405 with `Allow`),
request-binding deserialization (labels, query incl. `@httpQueryParams`, headers, JSON/CBOR
bodies), and response serialization (status, headers, body) are all generated; rpcv2Cbor
services check the `smithy-protocol` header and dispatch on the fixed
`/service/{Service}/operation/{Operation}` form. jsonRpc2 services register a single `POST /`
route and dispatch on the request envelope's `method` member, answering every well-formed
JSON-RPC call — success or error — as an HTTP 200 envelope (envelope-level failures use the
reserved JSON-RPC codes: -32700 parse, -32600 invalid request, -32601 method not found,
-32602 invalid params).

## Serving event streams

Event-stream operations (ADR-0016) grow the handler a streaming method — input first, the
typed session in the middle, context last — that blocks for the session's lifetime:
`Send`/`Receive` until done, then return `Unit` for a clean close, or an `Error`, which ends
the stream with one typed exception message before the close (a `Modeled` error with
`set_detail()` surfaces on the client exactly like a unary modeled error) — unless the
stream already terminated: propagating a failed `Receive()` closes without a message,
which that peer already observed. The `stream&` is valid only until the handler returns,
so join any helper thread still using it before returning. The generated
server exposes `StreamRouter()`, a `smithy::server::WebSocketRouter` with every streaming
route registered; mount it on the transport in two lines — the upgrade path deliberately
bypasses the HTTP middleware chain (ADR-0015), so unary dispatch beside it is untouched:

```cpp
smithy::http::BeastServerTransport::Options options;
options.websocket_gate = server.StreamRouter()->Gate();  // 404/405 refusals pre-upgrade
options.on_websocket = server.StreamRouter()->Serve();   // blocking per-session dispatch
smithy::http::BeastServerTransport transport(options);
transport.Start(server.Handler());                       // unary routes, same port
```

Whatever method the operation models, upgrades are always GET on the modeled URI — a
WebSocket upgrade is a GET on the wire, and the generated routes register accordingly.

Application admission policy (auth, rate limits) composes by wrapping `Gate()`: run your
refusal first, then defer to the router's (`smithy/server/websocket_router.h` shows the
pattern). Constraint validation also guards streaming inputs, with one wrinkle: the labels,
query, and headers arrive on the upgrade request, which the transport accepts *before* the
route parses them — so a validation failure surfaces as a successful dial whose first
`Receive()` is a terminal `SerializationException`, not as a refused upgrade.

Note `Stop()`'s semantics from ADR-0015: it *aborts* live stream sessions rather
than draining them, so a graceful rollout drains application-side first. The recipe: keep
your own registry of live sessions (the handler adds the borrowed `stream` on entry and
removes it on exit, under a mutex), and on drain call `Close()` on each — it is idempotent
and safe from any thread, each blocked handler wakes and returns, and once the registry
empties, `Stop()` has nothing left to abort.

Handlers are testable without any transport (or Boost): `InMemoryWebSocketPair` plus an
injected dialer runs the full generated client↔server path in memory. The chat example's
e2e fixture is ~15 lines:

```cpp
auto server = std::make_shared<ChatServer>(std::make_shared<MyHandler>());
smithy::ClientConfig config;
config.websocket_dialer = [&](const smithy::http::WebSocketDialRequest& request)
    -> smithy::Outcome<std::shared_ptr<smithy::http::WebSocket>> {
  auto [near, far] = smithy::http::InMemoryWebSocketPair::Create();
  smithy::http::HttpRequest upgrade;                 // what a transport would deliver
  upgrade.method = "GET";
  upgrade.target = request.target;
  upgrade.headers = request.headers;
  serve_threads.emplace_back([serve = server->StreamRouter()->Serve(), upgrade,
                              session = far] { serve(upgrade, *session); });
  return near;
};
auto client = ChatClient::Create(std::move(config));  // client->Converse(...) streams in memory
```

(Join the serve threads in teardown after `Close()`ing the far ends.) The full-duplex chat
example ([examples/chat/](../examples/chat/)) is the working reference:
a generated client and server streaming both directions over real WebSockets, plus this
in-memory wiring for Boost-free tests.

## Generated smoke tests

Every generated module ships `tests/smoke_test.cc` (target `:smoke_test` in the module's
`tests/` package): the generated client calls the generated server over the loopback transport
— every operation round-trips a minimal valid value and one test proves modeled-error mapping.
It passes out of the box and is the natural place to start testing a real handler. Streaming
operations are skipped by the generated smoke/integration suites (a unary round trip has no
meaning for a session; their handler overrides are close-immediately stubs) — drive them the
way the chat example's e2e tests do.

## Constraint validation

Inputs are validated against the model's constraint traits after parsing and before your
handler runs: `@required` (top-level body/query/header members), `@length` (strings count
Unicode code points), `@range`, `@pattern`, `@uniqueItems`, and enum membership, recursively
through structures, unions, lists, and maps. Failures never reach the handler — the server
responds with the standard 400 `ValidationException` wire shape (`message` summary plus a
`fieldList` of per-member `{path, message}` entries, JSON-pointer paths like `/list/0`), with
the exact message formats the official validation conformance suite pins. `@internal` enum
members stay accepted on the wire but are omitted from the advertised value set.

`@pattern` evaluates on a linear-time NFA engine (`smithy/core/regex.h`), so no pattern/input
combination can backtrack catastrophically — the classic ReDoS pattern `^([0-9]+)+$` validates
request-sized inputs in microseconds instead of hanging the dispatch thread. The engine covers
the ECMA-262 subset Smithy patterns use; backreferences and lookaround (inherently
backtracking constructs) are rejected at generation time with an error naming the shape and
the fix.

## Conformance

Generated servers pass the official server-mode `httpRequestTests`/`httpResponseTests` suites
(across the alloy simpleRestJson and rpcv2Cbor suites) plus the `httpMalformedRequestTests` parser-strictness suite, all with a documented must-shrink exclusion list. Wire
requests parse into the exact modeled inputs — including `@httpPayload` (blob/string/enum raw
bodies, structure/union/document JSON bodies) and `@httpPrefixHeaders` — malformed ones are
rejected with the exact error identity (strict booleans, bounds-checked integers, strict
timestamps, single-member unions, `SerializationException`/`UnsupportedMediaTypeException`/
`NotAcceptableException` headers), and handler outputs/errors serialize to the exact wire
responses — `@httpResponseCode`, 204 No Content bodies suppressed, Content-Type (415) and
Accept (406) enforcement (blob payloads without `@mediaType` accept anything), and
all-query-params `@httpQueryParams` maps. Ambiguous route tables fail at generation time.
Routes for `@requestCompression` operations transparently gunzip request bodies arriving with
`Content-Encoding: gzip` (decompression is size-capped against bombs; malformed gzip is a 400
serialization error) — see [production-guide.md](production-guide.md).

## Not yet generated (Phase 5+)

Nested `@required` absences as `fieldList` entries and a server-strict serde variant (clients
must skip null dense-map values and accept UTC-offset timestamps in responses; servers share
that serde today), and `@streaming` blob payloads (see
[Current limitations](../README.md#current-limitations)). Event-stream operations generate
streaming handlers and a `StreamRouter()` (ADR-0016).
