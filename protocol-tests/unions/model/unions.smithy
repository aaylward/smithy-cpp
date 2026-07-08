$version: "2.0"

namespace compile.unions

/// The union member-type gauntlet (issue #56): SinkChoice in the roundtrip
/// fixture only exercises string/int/struct members, so the union x protocol
/// cells for blob, timestamp, list, map, enum, intEnum, and recursion were
/// untested. This service exists purely to generate those cells through the
/// same Bazel path the compile gauntlet uses; the suites in this package pin
/// each member type in all four directions per protocol.
service UnionGauntlet {
    version: "2026-07-08"
    operations: [EchoChoice]
}

@http(method: "POST", uri: "/choice")
operation EchoChoice {
    input := {
        @required
        id: String

        choice: BigUnion
    }

    output := {
        @required
        id: String

        choice: BigUnion
    }

    errors: [ChoiceRejected]
}

/// One member per shape kind the SinkChoice suites cannot reach.
union BigUnion {
    text: String
    flag: Boolean
    small: Integer
    big: Long
    ratio: Double
    data: Blob
    when: Timestamp
    names: StringList
    attributes: StringMap
    grade: Grade
    rank: Rank
    node: Node
}

enum Grade {
    PASS = "pass"
    FAIL = "fail"
}

intEnum Rank {
    FIRST = 1
    LAST = 99
}

/// A self-recursive structure as a union member. (Recursion through the
/// union itself — Node carrying a BigUnion — is rejected by the generator
/// with a diagnostic: std::variant needs complete alternatives.)
structure Node {
    @required
    label: String

    next: Node
}

list StringList {
    member: String
}

map StringMap {
    key: String
    value: String
}

/// The error-shape union cell: the __type discriminator rides next to the
/// union member in error payloads, which is exactly the case the serde's
/// exactly-one-member arithmetic must tolerate.
@error("client")
structure ChoiceRejected {
    @required
    message: String

    offending: BigUnion
}
