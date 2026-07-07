$version: "2.0"

namespace example.calculator

use smithy.cpp.protocols#jsonRpc2

/// A classic JSON-RPC service: one endpoint, the operation name in the
/// envelope's method member. Demonstrates smithy.cpp.protocols#jsonRpc2 end
/// to end; the interop wire test next to the generated module drives the
/// generated server with hand-rolled JSON-RPC 2.0 calls (as any off-the-shelf
/// JSON-RPC client would issue them) and the generated client against a
/// hand-rolled JSON-RPC peer.
@jsonRpc2
service Calculator {
    version: "2026-01-01"
    operations: [Add, Divide]
}

/// Adds two numbers.
@readonly
operation Add {
    input := {
        @required
        a: Double

        @required
        b: Double
    }

    output := {
        @required
        sum: Double
    }
}

/// Divides dividend by divisor; dividing by zero is a modeled error.
@readonly
operation Divide {
    input := {
        @required
        dividend: Double

        @required
        divisor: Double
    }

    output := {
        @required
        quotient: Double
    }

    errors: [DivisionByZero]
}

@error("client")
@httpError(422)
structure DivisionByZero {
    @required
    message: String
}
