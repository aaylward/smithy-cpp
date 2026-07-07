$version: "2.0"

namespace smithy.cpp.ruletest

use aws.protocols#restJson1

/// Minimal service the rule analysis tests generate against.
@restJson1
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
