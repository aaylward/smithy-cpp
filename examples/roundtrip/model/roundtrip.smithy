$version: "2.0"

namespace example.roundtrip

use aws.protocols#restJson1
use smithy.protocols#rpcv2Cbor

/// Kitchen-sink fixture for the Phase 5 integration matrix: the same shapes
/// served over restJson1 (with every supported HTTP binding) and rpcv2Cbor,
/// so random round-trips exercise both protocols' serde end to end.
@restJson1
@httpApiKeyAuth(name: "api-key", in: "query")
service RoundTripRest {
    version: "2026-01-01"
    operations: [PutSink, UploadAttachment, DescribeSink]
}

@rpcv2Cbor
service RoundTripRpc {
    version: "2026-01-01"
    operations: [PutSinkRpc]
}

/// Every binding location at once: label, query, @httpQueryParams, headers,
/// prefix headers, and a JSON body full of aggregate shapes.
@idempotent
@http(method: "PUT", uri: "/sinks/{sinkId}")
operation PutSink {
    input := {
        @required
        @httpLabel
        sinkId: SinkId

        @httpQuery("tag")
        tag: String

        @httpQuery("limit")
        limit: PageLimit

        @httpHeader("x-sink-priority")
        priority: Priority

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
    }

    errors: [SinkNotFound, SinkQuotaExceeded]
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

    errors: [SinkNotFound]
}

/// The RPC variant round-trips the same kitchen sink over CBOR.
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
