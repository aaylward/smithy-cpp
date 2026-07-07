$version: "2.0"

namespace smithy.cpp.ruletest

/// Minimal service the rule analysis tests generate against. Deliberately
/// protocol-agnostic: greeter_restjson1.smithy binds the protocol with
/// `apply`, so every rule test also exercises multi-file model assembly.
service Greeter {
    version: "2026-01-01"
    operations: [Greet]
}

@readonly
@http(method: "GET", uri: "/greet/{name}")
operation Greet {
    input := {
        @required
        @httpLabel
        name: String
    }

    output := {
        @required
        greeting: String
    }
}
