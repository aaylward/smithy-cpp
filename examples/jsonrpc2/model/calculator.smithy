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
    operations: [Add, Divide, Accumulate]
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
@idempotent
operation Divide {
    input := {
        @required
        dividend: Double

        @required
        divisor: Double

        /// Auto-filled with a UUIDv4 by the client when unset.
        @idempotencyToken
        requestToken: String
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

/// A running-total session over the JSON-RPC stream wire (ADR-0023): the
/// opening call's params seed the accumulator, each streamed term answers
/// with a running total, and exceeding the modeled limit ends the stream
/// with the Overflow error as the terminal response envelope.
operation Accumulate {
    input := {
        /// The accumulator's starting value; rides the opening envelope's
        /// params — a body-bound initial-request member (ADR-0016's seam,
        /// realized by this protocol).
        start: Double

        terms: Terms
    }

    output := {
        totals: Totals
    }

    errors: [Overflow]
}

@streaming
union Terms {
    add: Term
}

@streaming
union Totals {
    total: RunningTotal
}

structure Term {
    @required
    value: Double
}

structure RunningTotal {
    @required
    value: Double
}

/// The @error class default (400) — DivisionByZero above carries the
/// explicit @httpError face.
@error("client")
structure Overflow {
    @required
    message: String

    @required
    limit: Double
}
