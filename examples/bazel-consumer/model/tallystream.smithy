$version: "2.0"

namespace acme.tally

use smithy.cpp.protocols#jsonRpc2

/// jsonRpc2 stream acceptance model (ADR-0023): the smallest bidirectional
/// service on the JSON-RPC-native wire, generated inside this module's
/// build graph like the todo and chat services beside it — the consumer
/// proof that the shared endpoint, the opening envelope's initial-request
/// members, and the transport's raw-text mode all resolve from a Bazel
/// consumer.
@jsonRpc2
service Tally {
    version: "2026-07-21"
    operations: [Count]
}

/// A running count: the opening call's params seed it, each bump answers
/// the new total, a zero bump ends the session cleanly (the terminal
/// result envelope), and counting below zero ends it with the modeled
/// Busted (the terminal error envelope).
operation Count {
    input := {
        /// Rides the opening envelope's params — a body-bound
        /// initial-request member (ADR-0016's seam, realized).
        start: Integer

        events: Bumps
    }

    output := {
        events: Totals
    }

    errors: [Busted]
}

@streaming
union Bumps {
    bump: Bump
}

@streaming
union Totals {
    total: Total
}

structure Bump {
    @required
    by: Integer
}

structure Total {
    @required
    value: Integer
}

@error("client")
structure Busted {
    @required
    message: String
}
