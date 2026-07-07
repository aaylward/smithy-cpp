# Server guide: implementing a generated service

Phase 4 generates a server counterpart for every service: `server.h`/`src/server.cc` with a
`<Service>Handler` interface and a `<Service>Server`, exposed as the module's `:server` Bazel
target.

## Implementing a handler

```cpp
#include "example/weather/server.h"

class MyHandler final : public example::weather::WeatherHandler {
 public:
  smithy::Outcome<GetCityOutput> GetCity(const GetCityInput& input) override {
    if (/* not found */) {
      smithy::Error error = smithy::Error::Modeled("NoSuchResource", "no city: " + input.cityId);
      error.set_detail(NoSuchResource{.resourceType = "City"});  // serializes the typed body
      return error;
    }
    return GetCityOutput{...};
  }
  // ... one method per operation
};
```

- **Modeled errors**: return `smithy::Error::Modeled("<ErrorShapeName>", message)`. The server
  maps the code to the shape's `@httpError` status (else 400/`@error("server")` → 500) and the
  protocol's error identity — restJson1 sets the `X-Amzn-Errortype` header and serializes the
  detail as the body; rpcv2Cbor writes the fully qualified shape id as `__type` in the CBOR
  body. Attach the typed structure with `set_detail()` to serialize its members (restJson1
  `@httpHeader`-bound error members become response headers); the generic message is added
  only when the detail carries no message member of its own.
- **Validation/serialization errors** (including malformed request input the framework catches
  before your handler runs) map to 400; any other failure is a non-leaking 500
  `InternalFailure`.

## Running a server

The server is transport-agnostic: `Handler()` returns a `smithy::http::RequestHandler` that
plugs into any `HttpServerTransport`:

```cpp
example::weather::WeatherServer server(std::make_shared<MyHandler>());

smithy::http::BeastServerTransport transport(options);  // production (ADR-0006)
transport.Start(server.Handler());
// or smithy::http::Loopback for in-process tests, SocketHttpServer for the built-in listener.
```

Routing (method + URI pattern from `@http`, greedy labels, 404/405 with `Allow`),
request-binding deserialization (labels, query incl. `@httpQueryParams`, headers, JSON/CBOR
bodies), and response serialization (status, headers, body) are all generated; rpcv2Cbor
services check the `smithy-protocol` header and dispatch on the fixed
`/service/{Service}/operation/{Operation}` form.

## Generated smoke tests

Every generated module ships `tests/smoke_test.cc` (target `:smoke_test` in the module's
`tests/` package): the generated client calls the generated server over the loopback transport
— every operation round-trips a minimal valid value and one test proves modeled-error mapping.
It passes out of the box and is the natural place to start testing a real handler.

## Conformance

Generated servers pass the official server-mode `httpRequestTests`/`httpResponseTests` suites
(220 cases across restJson1 and rpcv2Cbor, with a documented must-shrink exclusion list): wire
requests parse into the exact modeled inputs, and handler outputs/errors serialize to the exact
wire responses — including `@httpResponseCode`, 415 Content-Type enforcement (parameters like
`; charset=utf-8` accepted), and all-query-params `@httpQueryParams` maps. Ambiguous route
tables fail at generation time.

## Not yet generated (Phase 4c+)

Constraint validation from traits (`@length`, `@range`, `@pattern`, ... → 400
`ValidationException` before the handler runs), the `httpMalformedRequestTests` suite, and the
bindings the client also lacks (`@httpPayload`, `@httpPrefixHeaders`).
