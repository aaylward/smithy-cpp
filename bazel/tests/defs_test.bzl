"""Analysis tests for the smithy_cpp_*_library rules (bazel/defs.bzl)."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_testing//lib:analysis_test.bzl", "analysis_test", "test_suite")

_NS = "include/smithy/cpp/ruletest"

def _files(target):
    return sorted([f.short_path for f in target[DefaultInfo].files.to_list()])

def _prefixed(gen_name, paths):
    return sorted(["bazel/tests/%s/%s" % (gen_name, p) for p in paths])

def _client_outputs_impl(env, target):
    env.expect.that_collection(_files(target)).contains_exactly(_prefixed(
        "greeter_client_smithy_gen",
        [
            _NS + "/types.h",
            _NS + "/serde.h",
            _NS + "/client.h",
            "src/serde.cc",
            "src/client.cc",
        ],
    ))
    action = env.expect.that_target(target).action_named("SmithyCppGenerate")
    action.argv().contains_at_least([
        "--service",
        "smithy.cpp.ruletest#Greeter",
        "--mode",
        "client",
        "--emit-build-file",
        "false",
    ])

    # Multi-file models: every srcs entry becomes its own --model flag (the
    # base model plus the `apply` protocol overlay assemble into one model).
    action.argv().contains_at_least([
        "--model",
        "bazel/tests/greeter.smithy",
        "--model",
        "bazel/tests/greeter_restjson1.smithy",
    ])

def _server_outputs_impl(env, target):
    env.expect.that_collection(_files(target)).contains_exactly(_prefixed(
        "greeter_server_smithy_gen",
        [
            _NS + "/types.h",
            _NS + "/serde.h",
            _NS + "/server.h",
            "src/serde.cc",
            "src/server.cc",
        ],
    ))
    env.expect.that_target(target).action_named("SmithyCppGenerate").argv().contains_at_least([
        "--mode",
        "server",
    ])

def _types_outputs_impl(env, target):
    env.expect.that_collection(_files(target)).contains_exactly(_prefixed(
        "greeter_types_smithy_gen",
        [_NS + "/types.h"],
    ))

def _client_cc_library_impl(env, target):
    # The wrapper is an ordinary cc_library whose headers resolve via the
    # generated include root, so dependents just #include <ns>/client.h.
    env.expect.that_bool(CcInfo in target).equals(True)
    quote_includes = target[CcInfo].compilation_context.quote_includes.to_list()
    system_includes = target[CcInfo].compilation_context.includes.to_list()
    found = False
    for path in quote_includes + system_includes:
        if path.endswith("bazel/tests/greeter_client_smithy_gen/include"):
            found = True
    env.expect.that_bool(found).equals(True)

def client_outputs_test(name):
    analysis_test(name = name, impl = _client_outputs_impl, target = "//bazel/tests:greeter_client_smithy_gen")

def server_outputs_test(name):
    analysis_test(name = name, impl = _server_outputs_impl, target = "//bazel/tests:greeter_server_smithy_gen")

def types_outputs_test(name):
    analysis_test(name = name, impl = _types_outputs_impl, target = "//bazel/tests:greeter_types_smithy_gen")

def client_cc_library_test(name):
    analysis_test(name = name, impl = _client_cc_library_impl, target = "//bazel/tests:greeter_client")

def defs_test_suite(name):
    test_suite(
        name = name,
        tests = [
            client_outputs_test,
            server_outputs_test,
            types_outputs_test,
            client_cc_library_test,
        ],
    )
