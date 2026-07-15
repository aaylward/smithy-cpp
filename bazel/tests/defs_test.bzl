"""Analysis tests for the smithy_cpp_*_library rules (bazel/defs.bzl)."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_testing//lib:analysis_test.bzl", "analysis_test", "test_suite")
load("@rules_testing//lib:truth.bzl", "matching")
load("//bazel:defs.bzl", "validation_for_testing")

_v = validation_for_testing

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
        "bazel/tests/greeter_simplerestjson.smithy",
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

# Unit tests for the validation helpers themselves: the full accept/reject
# matrix, table-driven. The analysis-failure tests below then prove the rule
# actually fails with these messages.

def test_is_identifier_matrix(env):
    for ok in ["a", "A", "_", "_x", "a1", "snake_case", "CamelCase9"]:
        env.expect.that_bool(_v.is_identifier(ok)).equals(True)
    for bad in ["", "9a", "a-b", "a.b", "a b", "a::b", "ü"]:
        env.expect.that_bool(_v.is_identifier(bad)).equals(False)

def test_namespace_error_accepts_valid_cpp_namespaces(env):
    for ns in ["acme", "acme::todo", "a1::b_2", "_x::y9", "smithy::cpp::ruletest"]:
        env.expect.that_str(_v.namespace_error(ns)).equals(None)

def test_namespace_error_suggests_colon_form_for_smithy_dots(env):
    env.expect.that_str(_v.namespace_error("acme.todo")).contains(
        'did you mean "acme::todo"?',
    )
    env.expect.that_str(_v.namespace_error("smithy.cpp.ruletest")).contains(
        'did you mean "smithy::cpp::ruletest"?',
    )

def test_namespace_error_names_the_bad_segment(env):
    cases = {
        "": '""',
        "::acme": '""',
        "acme-todo": '"acme-todo"',
        "acme.todo::v1": '"acme.todo"',  # has "::" too, so no dots suggestion
        "acme::": '""',
        "acme::9todo": '"9todo"',
    }
    for ns, segment in cases.items():
        env.expect.that_str(_v.namespace_error(ns)).contains(
            "segment %s is not a C++ identifier" % segment,
        )

def test_service_error_accepts_valid_shape_ids(env):
    for svc in ["acme.todo#Todo", "a#B", "smithy.cpp.ruletest#Greeter", "_ns._sub#_Svc9"]:
        env.expect.that_str(_v.service_error(svc)).equals(None)

def test_service_error_explains_the_shape_id_form(env):
    for svc in ["Todo", "a#b#c", "acme todo#Todo", "9a.b#C", "acme.todo#9Todo", "acme.todo#", "#Todo"]:
        env.expect.that_str(_v.service_error(svc)).contains(
            'expected "<namespace>#<ServiceName>"',
        )

def test_service_error_suggests_dotted_form_for_cpp_namespace(env):
    env.expect.that_str(_v.service_error("acme::todo#Todo")).contains(
        'did you mean "acme.todo#Todo"?',
    )
    env.expect.that_str(_v.service_error("smithy::cpp::ruletest#Greeter")).contains(
        'did you mean "smithy.cpp.ruletest#Greeter"?',
    )

# tags on the macro must reach the internal generate target (that forwarding is
# what keeps the misconfigured instances below out of wildcard builds).

def _tags_forwarded_impl(env, target):
    env.expect.that_target(target).tags().contains("manual")

def tags_forwarded_to_internal_targets_test(name):
    analysis_test(
        name = name,
        impl = _tags_forwarded_impl,
        target = "//bazel/tests:greeter_manual_types_smithy_gen",
    )

# Wiring mistakes fail at analysis time with the fix in the message; these
# pin both the failure and the message's actionable part.

def _expect_failure_containing(env, target, needle):
    env.expect.that_target(target).failures().contains_predicate(
        matching.str_matches("*" + needle + "*"),
    )

def _smithy_dots_namespace_impl(env, target):
    _expect_failure_containing(env, target, 'did you mean "smithy::cpp::ruletest"?')

def _bad_namespace_segment_impl(env, target):
    _expect_failure_containing(env, target, 'segment "9ruletest" is not a C++ identifier')

def _bare_service_name_impl(env, target):
    _expect_failure_containing(env, target, 'expected "<namespace>#<ServiceName>"')

def _cpp_namespace_service_impl(env, target):
    _expect_failure_containing(env, target, 'did you mean "smithy.cpp.ruletest#Greeter"?')

def smithy_dots_namespace_test(name):
    analysis_test(
        name = name,
        impl = _smithy_dots_namespace_impl,
        target = "//bazel/tests:greeter_smithy_dots_namespace_smithy_gen",
        expect_failure = True,
    )

def bad_namespace_segment_test(name):
    analysis_test(
        name = name,
        impl = _bad_namespace_segment_impl,
        target = "//bazel/tests:greeter_bad_namespace_segment_smithy_gen",
        expect_failure = True,
    )

def bare_service_name_test(name):
    analysis_test(
        name = name,
        impl = _bare_service_name_impl,
        target = "//bazel/tests:greeter_bare_service_name_smithy_gen",
        expect_failure = True,
    )

def cpp_namespace_service_test(name):
    analysis_test(
        name = name,
        impl = _cpp_namespace_service_impl,
        target = "//bazel/tests:greeter_cpp_namespace_service_smithy_gen",
        expect_failure = True,
    )

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
            tags_forwarded_to_internal_targets_test,
            smithy_dots_namespace_test,
            bad_namespace_segment_test,
            bare_service_name_test,
            cpp_namespace_service_test,
        ],
        basic_tests = [
            test_is_identifier_matrix,
            test_namespace_error_accepts_valid_cpp_namespaces,
            test_namespace_error_suggests_colon_form_for_smithy_dots,
            test_namespace_error_names_the_bad_segment,
            test_service_error_accepts_valid_shape_ids,
            test_service_error_explains_the_shape_id_form,
            test_service_error_suggests_dotted_form_for_cpp_namespace,
        ],
    )
