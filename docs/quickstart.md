# Quick start: model → client + server in one Bazel module

From an empty directory to a generated C++ client integration-testing a generated C++ server,
without touching the generator's internals. The finished result of every step below lives at
[`examples/bazel-consumer/`](../examples/bazel-consumer) — CI builds that module standalone on
every commit, so this tutorial cannot silently rot.

## 1. Create a Bazel module

`MODULE.bazel`:

```starlark
module(name = "my_service", version = "0.0.0")

bazel_dep(name = "smithy_cpp", version = "0.0.0")

# Until smithy_cpp is published to the Bazel Central Registry (deferred until
# the project is production-validated), consume it by path or git override:
git_override(
    module_name = "smithy_cpp",
    remote = "https://github.com/aaylward/smithy-cpp.git",
    commit = "<pin a commit>",
)

bazel_dep(name = "googletest", version = "1.17.0.bcr.2")
bazel_dep(name = "rules_cc", version = "0.2.17")
```

`.bazelrc` (C++20 is the runtime baseline; the generator runs on a hermetic Java 17 toolchain):

```
common --enable_platform_specific_config
build:linux --cxxopt=-std=c++20 --host_cxxopt=-std=c++20
build:macos --cxxopt=-std=c++20 --host_cxxopt=-std=c++20
build:windows --cxxopt=/std:c++20 --host_cxxopt=/std:c++20
common --java_language_version=17
common --tool_java_language_version=17
common --java_runtime_version=remotejdk_17
common --tool_java_runtime_version=remotejdk_17
```

## 2. Write a model

`model/todo.smithy` — a service, an operation or two, a modeled error. Keep the model
protocol-agnostic, the upstream Smithy way: `@http` traits describe HTTP semantics without
picking a wire protocol. Bind a concrete protocol in a small overlay file with `apply`:

```smithy
// model/bindings/simplerestjson.smithy
$version: "2.0"
namespace acme.todo
use alloy#simpleRestJson
apply Todo @simpleRestJson
```

(Applying the trait directly on the service works too, if you only ever want one protocol.)

## 3. Declare the generated libraries

`BUILD.bazel` — pass the base model plus the overlay that picks the protocol:

```starlark
load("@smithy_cpp//bazel:defs.bzl", "smithy_cpp_client_library", "smithy_cpp_server_library")

smithy_cpp_client_library(
    name = "todo_client",
    srcs = [
        "model/bindings/simplerestjson.smithy",
        "model/todo.smithy",
    ],
    namespace = "acme::todo",
    service = "acme.todo#Todo",
)

smithy_cpp_server_library(
    name = "todo_server",
    srcs = [
        "model/bindings/simplerestjson.smithy",
        "model/todo.smithy",
    ],
    namespace = "acme::todo",
    service = "acme.todo#Todo",
)
```

Because the protocol lives in the overlay, the same model generates for another protocol by
swapping the overlay — the consumer example binds `acme.todo#Todo` to simpleRestJson, rpcv2Cbor,
**and** jsonRpc2 side by side (different `namespace` per binding keeps the headers apart); see
[`examples/bazel-consumer/BUILD.bazel`](../examples/bazel-consumer/BUILD.bazel).

Generation runs inside the build graph as a hermetic action — correct caching, no scripts, no
Gradle. Each target is an ordinary `cc_library`: depend on it, `#include "acme/todo/client.h"`,
done. (`smithy_cpp_types_library` exists too, for data types without a protocol.)

## 4. Implement the handler and test it with the generated client

The server library gives you a pure-virtual `TodoHandler` and a `TodoServer`; the client library
a `TodoClient`. Wire them together over the in-memory loopback (or a real socket) exactly like
[`todo_integration_test.cc`](../examples/bazel-consumer/todo_integration_test.cc):

```cpp
TodoServer server(std::make_shared<MyHandler>());
auto loopback = std::make_shared<smithy::http::Loopback>();
(void)loopback->Start(server.Handler());
smithy::ClientConfig config;
config.http_client = loopback;
auto client = *TodoClient::Create(std::move(config));
```

Routing, serde, constraint validation (400 `ValidationException` before your handler runs),
content negotiation, and modeled-error mapping are all generated — see
[server-guide.md](server-guide.md) for what the server does on your behalf.

## 5. Run it

```sh
bazel test //...
```

For production serving, plug `server.Handler()` into `smithy::http::BeastServerTransport`
(`@smithy_cpp//runtime:http_beast`, ADR-0006).

## Generating outside Bazel

The generator is also a plain CLI for inspecting output or vendoring generated sources:

```sh
bazel run @smithy_cpp//codegen:generator -- \
    --model $PWD/model/todo.smithy --service acme.todo#Todo \
    --namespace acme::todo --mode both --output /tmp/generated
```

`--mode types|client|server|both` picks what to emit; `--emit-build-file false` suppresses the
generated `BUILD.bazel` when you're writing your own.

## Day 2: evolving the model

Once the integration is running, the model keeps changing — new fields, new operations,
tightened constraints. [model-evolution.md](model-evolution.md) covers that loop: how edits
propagate through the build graph (or through regeneration for vendored output), how to review
generated diffs, and how CI catches drift and unimplemented handler methods.
