$version: "2.0"

namespace compile.streaming

use alloy#simpleRestJson
use smithy.cpp.protocols#jsonRpc2
use smithy.protocols#rpcv2Cbor

/// The streaming compile gauntlet (ADR-0016): a bidirectional chat-like
/// operation plus a server-push one, so the generated EventStream signatures,
/// codec dispatch, exception paths, and NoEvents directions all compile.
///
/// Two services, not one: the protocols disagree on what an upgrade request
/// can carry. simpleRestJson resolves label/query/header initial members onto
/// the upgrade GET; rpcv2Cbor upgrades on its fixed URI and rejects initial
/// members at generation time, so its operations keep stream-only inputs.
///
/// The auth traits pull the client's auth wiring onto the upgrade dial
/// (bearer here, api-key on Pipe), and the @length on Converse's room label
/// exercises the streaming route's validation-refusal arm — both asserted
/// behaviorally by streaming_compile_test.
@httpBearerAuth
@simpleRestJson
service Relay {
    version: "2026-07-19"
    operations: [Converse, Watch]
}

/// rpcv2Cbor face of the same streaming shapes.
@httpApiKeyAuth(name: "x-api-key", in: "header")
@rpcv2Cbor
service Pipe {
    version: "2026-07-19"
    operations: [Exchange, Watch]
}

/// jsonRpc2 face (ADR-0023): the shared endpoint carries the opening
/// envelope, so initial-request members ride its params — including the
/// constrained one, which exercises the stream drivers'
/// validation-refusal arm (no other fixture drives it on this wire). The
/// shared Watch rides along for the params-less opening (Unit input) and
/// the NoEvents transmit direction.
@httpBearerAuth
@jsonRpc2
service Wire {
    version: "2026-07-19"
    operations: [Confer, Watch]
}

/// Bidirectional on the JSON-RPC stream wire: no @http anything — the
/// operation name IS the routing, in the opening envelope's method.
operation Confer {
    input := {
        @length(max: 8)
        room: String

        events: ClientEvents
    }

    output := {
        events: ServerEvents
    }

    errors: [RoomGone]
}

/// Bidirectional: initial-request members ride the upgrade URI and headers.
@http(method: "POST", uri: "/rooms/{room}/converse")
operation Converse {
    input := {
        @required
        @httpLabel
        @length(max: 8)
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
