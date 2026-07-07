# Generated types: the Smithy → C++ mapping contract

What `cpp-codegen` emits per shape (Phase 2: data types; Phase 3: serde + clients). This is a
compatibility contract: changes to it are breaking for consumers of generated code.

## Type mapping

| Smithy shape | C++ type | Notes |
|---|---|---|
| `boolean` | `bool` | |
| `byte` / `short` / `integer` / `long` | `std::int8_t` / `std::int16_t` / `std::int32_t` / `std::int64_t` | |
| `float` / `double` | `float` / `double` | |
| `string` | `std::string` | |
| `blob` | `smithy::Blob` | |
| `timestamp` | `smithy::Timestamp` | |
| `document` | `smithy::Document` | |
| `list<T>` | `std::vector<T>` | `@sparse` ⇒ `std::vector<std::optional<T>>` |
| `map<string, T>` | `std::map<std::string, T>` | `@sparse` ⇒ optional values; `std::map` keeps output deterministic |
| `structure` | `struct` with public members | Aggregate; `friend operator== = default`; every member value-initialized with `{}` |
| `union` | class over `std::variant` | See below |
| `enum` | class with nested `enum class Value` | See below; unknown wire values preserved |
| `intEnum` | `enum class X : std::int32_t` | |
| `smithy.api#Unit` | `smithy::Unit` | Never declared; maps to the runtime type |
| `bigInteger` / `bigDecimal` | — | Rejected with a clear error (planned) |
| recursive structures/unions | — | Rejected with a clear error (boxed recursion planned) |

## Conventions

- **Names**: shape names are used as-is (PascalCase by Smithy convention); member names are used
  as-is (camelCase). C++ keywords get a trailing underscore (`namespace` → `namespace_`).
- **Optionality**: `@required` members map to the plain type; everything else is
  `std::optional<T>`.
- **Docs**: `@documentation` becomes `///` comments.
- **Files**: per module, `include/<namespace path>/types.h`, `serde.h` + `src/serde.cc`,
  `client.h` + `src/client.cc`, and a generated `BUILD.bazel` exposing `cc_library ":types"`
  and `":client"` targets that depend on the smithy-cpp runtime (`runtimeTarget` /
  `runtimePackage` settings).

## Enums

```cpp
class CoffeeType {
 public:
  enum class Value { kDrip, kEspresso, kUnknown };
  CoffeeType(Value value);                          // implicit, by design
  static CoffeeType FromString(std::string_view);   // unknown text => Value::kUnknown
  Value value() const;
  std::string_view ToString() const;                // unknown values keep their original text
};
```

Constants are `k` + PascalCase of the member name (`DRIP` → `kDrip`, `OAT_MILK` → `kOatMilk`).
Unknown-value preservation means a round trip through an old client never corrupts data written
by a newer service.

## Unions

```cpp
class MilkOption {
 public:
  MilkOption();                                   // empty() until a factory is used
  static MilkOption FromDairy(DairyMilk value);   // From + PascalCase(member)
  bool is_dairy() const;                          // is_<member>
  const DairyMilk& as_dairy() const;              // as_<member>; UB if not set (variant rules)
  bool empty() const;
};
```

Backed by `std::variant<std::monostate, ...members>` — index-addressed, so duplicate member
target types are fine.

## Serde (Phase 3)

`serde.h`/`src/serde.cc` emit one pair of free functions per aggregate shape reachable from an
operation:

```cpp
smithy::Document SerializeOrderCoffeeInput(const OrderCoffeeInput& value);
smithy::Outcome<OrderCoffeeInput> DeserializeOrderCoffeeInput(const smithy::Document& value);
```

- **Pivot type**: `smithy::Document` — protocol-independent; the client picks the JSON or CBOR
  codec at the wire boundary, so serde is generated once per shape, not once per protocol.
- **Tolerant reads**: unknown response members are ignored; unknown enum values are preserved
  (`Value::kUnknown` + original text). Missing `@required` members produce a
  `smithy::ErrorKind::kSerialization` error naming the member.
- **Sparse** lists/maps serialize `std::nullopt` as explicit nulls; timestamps honor
  `@timestampFormat` with the protocol default applied where unspecified.

## Clients (Phase 3)

`client.h`/`src/client.cc` emit a `<Service>Client` per service:

```cpp
auto client = WeatherClient::Create(std::move(config));   // Outcome; validates config
auto city   = client->GetCity(GetCityInput{.cityId = "seattle"});  // Outcome<GetCityOutput>
```

- **Transport-agnostic**: `smithy::ClientConfig` supplies either an `endpoint` (uses the default
  socket transport) or an explicit `http_client` (loopback for tests, Beast, custom).
- **Protocol binding** is chosen at generation time from the service's protocol trait:
  restJson1 (HTTP bindings: labels, query, headers, status codes) or rpcv2Cbor
  (`POST /service/{S}/operation/{O}`, `smithy-protocol: rpc-v2-cbor`, CBOR bodies).
- `@idempotencyToken` members are auto-filled with a UUIDv4 when unset; caller-provided values
  pass through untouched.
- HTTP 4xx/5xx map to `smithy::ErrorKind::kModeled` with the sanitized error code
  (`ns#Shape` → `Shape`, restJson1 also reads the `x-amzn-errortype` header); `@retryable`
  errors and 5xx responses are marked retryable.
- **Typed errors**: when the code matches an error the operation declares, the deserialized
  error structure rides along as the error's detail —
  `if (const auto* e = outcome.error().detail<OrderNotFound>()) use(e->orderId);`
  Undeclared codes still surface generically (code + message, no detail).
- restJson1 honors `@jsonName` body keys, serializes non-finite numbers as
  `"NaN"`/`"Infinity"`/`"-Infinity"`, and binds response `@httpHeader` members (including
  comma-joined lists and base64 `@mediaType` strings).

## Servers (Phase 4)

`server.h`/`src/server.cc` emit a pure-virtual `<Service>Handler` (one `Outcome`-returning
method per operation) and a `<Service>Server` that binds it to the runtime router; `Handler()`
returns a transport-agnostic `smithy::http::RequestHandler`. Routing, binding deserialization,
response serialization, and modeled-error mapping (`@httpError` status, `__type` body, typed
detail via `set_detail`) are generated — see [docs/server-guide.md](server-guide.md). Every
module also gets `tests/smoke_test.cc`: generated client ↔ generated server over loopback.

## Naming

- Cross-namespace name collisions in a service closure are disambiguated by appending the
  foreign namespace's last segment (`shared#Greeting` → `GreetingShared`); the service's own
  namespace keeps plain names, and wire-level error codes always use the Smithy shape name.
