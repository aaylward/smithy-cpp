$version: "2.0"

namespace compile.streaming

use alloy#simpleRestJson
use smithy.protocols#rpcv2Cbor

/// The streaming compile gauntlet (ADR-0016): a bidirectional chat-like
/// operation plus a server-push one, so the generated EventStream signatures,
/// codec dispatch, exception paths, and NoEvents directions all compile.
///
/// Two services, not one: the protocols disagree on what an upgrade request
/// can carry. simpleRestJson resolves label/query/header initial members onto
/// the upgrade GET; rpcv2Cbor upgrades on its fixed URI and rejects initial
/// members at generation time, so its operations keep stream-only inputs.
@simpleRestJson
service Relay {
    version: "2026-07-19"
    operations: [Converse, Watch]
}

/// rpcv2Cbor face of the same streaming shapes; jsonRpc2 gets no binding —
/// it refuses event streams with a generation-time diagnostic.
@rpcv2Cbor
service Pipe {
    version: "2026-07-19"
    operations: [Exchange, Watch]
}

/// Bidirectional: initial-request members ride the upgrade URI and headers.
@http(method: "POST", uri: "/rooms/{room}/converse")
operation Converse {
    input := {
        @required
        @httpLabel
        room: String

        @httpQuery("since")
        since: Integer

        @httpHeader("x-relay-client")
        client: String

        @httpPayload
        events: ClientEvents
    }
    output := {
        @httpPayload
        events: ServerEvents
    }
    errors: [RoomGone]
}

/// Bidirectional over the fixed rpcv2Cbor upgrade URI: stream-only input.
@http(method: "POST", uri: "/exchange")
operation Exchange {
    input := {
        @httpPayload
        events: ClientEvents
    }
    output := {
        @httpPayload
        events: ServerEvents
    }
    errors: [RoomGone]
}

/// Server-push: no input stream, so the client transmit direction is the
/// runtime's NoEvents. Shared by both services (no input members at all).
@readonly
@http(method: "GET", uri: "/watch")
operation Watch {
    output := {
        @httpPayload
        events: ServerEvents
    }
}

@streaming
union ClientEvents {
    message: ChatMessage
    typing: TypingNotice
}

@streaming
union ServerEvents {
    message: ChatMessage
    joined: MemberJoined
}

structure ChatMessage {
    @required
    text: String

    sender: String
}

structure TypingNotice {
    sender: String
}

structure MemberJoined {
    @required
    member: String
}

@error("client")
@httpError(410)
structure RoomGone {
    message: String
}
