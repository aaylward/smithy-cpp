# Generated types: the Smithy → C++ mapping contract

What `cpp-codegen` emits per shape (Phase 2 scope: data types). This is a compatibility
contract: changes to it are breaking for consumers of generated code.

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
- **Files**: one `include/<namespace path>/types.h` per module plus a generated `BUILD.bazel`
  exposing `cc_library ":types"` that depends on the smithy-cpp runtime (`runtimeTarget`
  setting).

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
target types are fine. Unknown-member tolerance on the wire is a Phase 3 serde concern.
