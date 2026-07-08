$version: "2.0"

namespace compile.gauntlet

/// The compile gauntlet: legal-but-unusual Smithy that historically broke the
/// generated C++ (issue #43's escaping / name-collision class). Nothing here
/// appears in the example fixtures, so this model is what stands between a
/// regression in the generator's escaping and a consumer's broken build.
///
/// Doc-comment hostility rides along: a stray */ sequence, a backslash \,
/// "double quotes", <angle brackets>, and a $dollar sign.
service Gauntlet {
    version: "2026-07-08"
    operations: [RunGauntlet, GetReport]
}

/// Members named after C++ keywords, int64 extremes, hostile enum values, and
/// recursive/union/collection shapes — all in one validated input so the
/// server-side ValidationGenerator paths fire too.
@http(method: "POST", uri: "/gauntlet")
operation RunGauntlet {
    input := {
        /// Pattern text carrying a double quote and a backslash: both must
        /// survive into the generated matcher and its violation message.
        @required
        @length(min: 1, max: 64)
        @pattern("^[A-Za-z0-9\"\\\\ ]{1,64}$")
        name: String

        // Every one of these is a C++ keyword the generator must escape.
        class: String
        namespace: String
        template: String
        operator: Boolean
        delete: Boolean
        int: Integer
        double: Double
        union: String
        default: String
        friend: String
        this: String
        auto: String
        register: Integer

        // Not keywords, but they shadow names generated code likes to use.
        value: String
        kind: String
        _leadingUnderscore: String

        /// int64 extremes: the minimum is unrepresentable as a plain C++
        /// literal (negating it overflows) and must be emitted as an
        /// expression (issue #43).
        @range(min: -9223372036854775808, max: 9223372036854775807)
        extremes: Long

        /// The same hazard through @default.
        floor: Long = -9223372036854775808

        ceiling: Long = 9223372036854775807

        /// Enum-targeted validated member: the "expected one of" message
        /// quotes every hostile wire value below.
        @required
        coffee: HostileEnum

        weight: HostileIntEnum

        choice: HostileUnion

        @idempotencyToken
        token: String

        payload: Blob

        when: Timestamp

        tags: TagList

        holes: SparseTagList

        attributes: AttributeMap

        tree: Node
    }

    output := {
        @required
        coffee: HostileEnum

        choice: HostileUnion

        tree: Node
    }

    errors: [GauntletRejected]
}

/// REST-binding stress: label, query, and header members whose names need
/// escaping on the C++ side while keeping their wire spelling.
@readonly
@http(method: "GET", uri: "/gauntlet/{class}")
operation GetReport {
    input := {
        @required
        @httpLabel
        class: String

        @httpQuery("switch")
        switch: String

        @httpHeader("x-gauntlet-case")
        case: String
    }

    output := {
        @required
        @httpResponseCode
        status: Integer

        report: String
    }
}

/// Wire values that must be escaped wherever they are quoted: in serde string
/// literals and in the validation "expected one of" message (issue #43's
/// CRITICAL case). TRICKY_RAW is the raw-string delimiter attack.
enum HostileEnum {
    QUOTE = "he said \"more\""
    BACKSLASH = "C:\\temp\\new"
    NEWLINE = "line one\nline two"
    TRICKY_RAW = ")__smithy\""
    UNICODE_VALUE = "café"
}

intEnum HostileIntEnum {
    BOTTOM = -2147483648
    TOP = 2147483647
    NOTHING = 0
}

/// Keyword-named members again, this time as union variants (factory and
/// accessor names derive from them), plus a recursive branch.
union HostileUnion {
    int: Integer
    class: String
    blob: Blob
    node: Node
}

/// Recursive through both a direct member and a list.
structure Node {
    @required
    label: String

    next: Node

    children: NodeList
}

list NodeList {
    member: Node
}

list TagList {
    member: String
}

@sparse
list SparseTagList {
    member: String
}

map AttributeMap {
    key: String
    value: String
}

@error("client")
@httpError(422)
structure GauntletRejected {
    @required
    message: String

    /// A keyword member on an error shape.
    class: String
}
