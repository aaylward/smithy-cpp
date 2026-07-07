$version: "2.0"

namespace example.keyvalue

use smithy.cpp.protocols#jsonRpc2

/// A tiny key/value store over the vendor-neutral JSON-RPC 2.0 protocol.
/// Every operation POSTs to one endpoint; the method field dispatches.
@jsonRpc2
service KeyValue {
    version: "2026-07-07"
    operations: [GetValue, PutValue]
}

@readonly
operation GetValue {
    input := {
        @required
        @length(min: 1, max: 128)
        key: String
    }

    output := {
        @required
        key: String

        @required
        value: String
    }

    errors: [NoSuchKey]
}

operation PutValue {
    input := {
        @required
        key: String

        @required
        value: String

        @idempotencyToken
        requestId: String
    }

    output := {
        @required
        key: String
    }
}

@error("client")
structure NoSuchKey {
    @required
    message: String

    @required
    key: String
}
