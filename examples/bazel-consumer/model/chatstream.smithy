$version: "2.0"

namespace acme.chat

use alloy#simpleRestJson

/// Streaming acceptance model (ADR-0016, slice 3's consumer e2e): the
/// smallest bidirectional event-stream service, generated inside this
/// module's build graph like the todo service beside it. simpleRestJson
/// carries the protocol inline — the generator's streaming support is
/// per-protocol, so no protocol-agnostic base + overlay split here.
@simpleRestJson
service Chat {
    version: "2026-07-19"
    operations: [Exchange]
}

/// Bidirectional echo: notes up, notes down, one WebSocket session.
@http(method: "POST", uri: "/exchange")
operation Exchange {
    input := {
        @httpPayload
        events: Notes
    }
    output := {
        @httpPayload
        events: Notes
    }
}

@streaming
union Notes {
    note: Note
}

structure Note {
    @required
    text: String
}
