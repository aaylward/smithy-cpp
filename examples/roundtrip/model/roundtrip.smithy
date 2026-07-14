$version: "2.0"

namespace example.roundtrip

use alloy#simpleRestJson
use smithy.cpp.protocols#jsonRpc2
use smithy.protocols#rpcv2Cbor

/// Kitchen-sink fixture for the Phase 5 integration matrix: the same shapes
/// served over simpleRestJson (with every supported HTTP binding), rpcv2Cbor,
/// and jsonRpc2, so random round-trips exercise every protocol's serde end
/// to end.
@simpleRestJson
@httpApiKeyAuth(name: "api-key", in: "query")
service RoundTripRest {
    version: "2026-01-01"
    operations: [PutSink, UploadAttachment, DescribeSink]
}

@rpcv2Cbor
service RoundTripRpc {
    version: "2026-01-01"
    operations: [PutSinkRpc, Ping]
}

/// No-input, no-output operation: exists so the hand-written wire test can
/// pin that the rpcv2Cbor server ignores request bodies sent to a no-input
/// operation (issue #68 — the upstream conformance suite carries no such
/// case, and #67 fixed a client/server asymmetry exactly here).
operation Ping {}

/// The same RPC surface as RoundTripRpc, served over JSON-RPC 2.0: one model,
/// three protocol variants.
@jsonRpc2
service RoundTripJsonRpc {
    version: "2026-01-01"
    operations: [PutSinkRpc]
}

/// Every binding location at once: label, query, @httpQueryParams, headers,
/// prefix headers, and a JSON body full of aggregate shapes. Compressed and
/// carrying required query/header members so the HTTP+JSON gzip path and the
/// required-absence validation wiring both land in a compiled golden
/// (issue #68: conditional emissions need fixtures on both branches).
@idempotent
@http(method: "PUT", uri: "/sinks/{sinkId}")
@requestCompression(encodings: ["gzip"])
operation PutSink {
    input := {
        @required
        @httpLabel
        sinkId: SinkId

        @httpQuery("tag")
        tag: String

        @required
        @httpQuery("limit")
        limit: PageLimit

        @httpHeader("x-sink-priority")
        priority: Priority

        @required
        @httpHeader("x-sink-created")
        created: Timestamp

        @httpPrefixHeaders("x-meta-")
        metadata: StringMap

        sink: KitchenSink

        // rpcv2Cbor forbids document types, so the document member lives on
        // the REST operation rather than the shared KitchenSink.
        freeform: Document
    }

    output := {
        @required
        sinkId: String

        @httpHeader("x-sink-revision")
        revision: Integer

        @httpPrefixHeaders("x-echo-")
        echoedMetadata: StringMap

        sink: KitchenSink

        // Deliberately named after the operation: server.cc's
        // BuildPutSinkResponse helper and serde's SerializePutSinkResponse
        // must coexist in one translation unit (issue #64 — helper names
        // stay out of the serde Serialize/Deserialize<Shape> pattern).
        echo: PutSinkResponse
    }

    errors: [SinkNotFound, SinkQuotaExceeded]
}

/// Named after the PutSink operation on purpose — see the `echo` member.
structure PutSinkResponse {
    note: String
}

/// Raw blob payload with an extra header member.
@http(method: "POST", uri: "/sinks/{sinkId}/attachment")
operation UploadAttachment {
    input := {
        @required
        @httpLabel
        sinkId: SinkId

        @httpHeader("x-attachment-name")
        name: String

        @httpPayload
        data: Blob
    }

    output := {
        @httpPayload
        receipt: Receipt
    }

    errors: [SinkNotFound]
}

/// Read-only operation: no body in, body out.
@readonly
@http(method: "GET", uri: "/sinks/{sinkId}")
operation DescribeSink {
    input := {
        @required
        @httpLabel
        sinkId: SinkId
    }

    output := {
        sink: KitchenSink
    }

    errors: [SinkNotFound, DescribeSinkError]
}

/// Deliberately named after the operation: client.cc's ParseDescribeSinkError
/// helper and serde's DeserializeDescribeSinkError (called inside
/// MakeDescribeSinkErrorError, same translation unit) must coexist
/// (issue #64 — helper names stay out of the serde naming pattern).
@error("client")
@httpError(410)
structure DescribeSinkError {
    @required
    message: String
}

/// The RPC variant round-trips the same kitchen sink over CBOR — compressed,
/// so the rpcv2Cbor decompress path and jsonRpc2's shared-endpoint
/// anyCompressed branch both land in compiled goldens (issue #68).
@requestCompression(encodings: ["gzip"])
operation PutSinkRpc {
    input := {
        @required
        sinkId: String

        sink: KitchenSink
    }

    output := {
        @required
        sinkId: String

        sink: KitchenSink
    }

    errors: [SinkNotFound, SinkQuotaExceeded]
}

@pattern("^[A-Za-z0-9]+$")
@length(min: 1, max: 32)
string SinkId

@range(min: 1, max: 100)
integer PageLimit

enum Priority {
    LOW = "low"
    MEDIUM = "medium"
    HIGH = "high"
}

intEnum Weight {
    LIGHT = 1
    HEAVY = 2
}

structure KitchenSink {
    @required
    name: String

    flag: Boolean
    tiny: Byte
    small: Short
    medium: Integer
    big: Long
    ratio: Float
    precise: Double
    blob: Blob
    priority: Priority
    weight: Weight

    dateTime: DateTimeStamp
    httpDate: HttpDateStamp
    epoch: EpochStamp

    names: StringList
    uniqueNames: UniqueStringList
    sparseNumbers: SparseIntegerList
    attributes: StringMap
    nested: NestedConfig
    choice: SinkChoice
}

structure NestedConfig {
    @required
    label: String

    depth: Integer
}

union SinkChoice {
    text: String
    count: Integer
    nested: NestedConfig
}

@timestampFormat("date-time")
timestamp DateTimeStamp

@timestampFormat("http-date")
timestamp HttpDateStamp

@timestampFormat("epoch-seconds")
timestamp EpochStamp

list StringList {
    member: String
}

@uniqueItems
list UniqueStringList {
    member: String
}

@sparse
list SparseIntegerList {
    member: Integer
}

map StringMap {
    key: String
    value: String
}

structure Receipt {
    @required
    receiptId: String

    size: Long
}

@error("client")
@httpError(404)
structure SinkNotFound {
    @required
    message: String

    resourceType: String
}

@error("server")
@httpError(503)
structure SinkQuotaExceeded {
    message: String

    @httpHeader("x-retry-after-seconds")
    retryAfterSeconds: Integer
}
