$version: "2.0"

namespace example.cafe

use smithy.protocols#rpcv2Cbor

/// Takes and tracks coffee orders.
///
/// RPC fixture model: exercises the rpcv2Cbor protocol plus enums, unions,
/// idempotency tokens, and modeled errors. A streaming operation is added in
/// Phase 8 (docs/PLAN.md); until then this file intentionally has no
/// @streaming shapes.
@rpcv2Cbor
@title("Cafe Service")
@httpApiKeyAuth(name: "x-api-key", in: "header")
service Cafe {
    version: "2026-07-06"
    operations: [OrderCoffee, GetOrder]
}

@requestCompression(encodings: ["gzip"])
operation OrderCoffee {
    input := {
        @required
        coffeeType: CoffeeType

        milk: MilkOption

        @idempotencyToken
        clientToken: ClientToken
    }

    output := {
        @required
        orderId: OrderId

        @required
        status: OrderStatus
    }

    errors: [OutOfBeans]
}

@readonly
operation GetOrder {
    input := {
        @required
        orderId: OrderId
    }

    output := {
        @required
        orderId: OrderId

        @required
        coffeeType: CoffeeType

        @required
        status: OrderStatus
    }

    errors: [OrderNotFound]
}

@length(min: 1, max: 128)
string OrderId

/// Idempotency tokens are secret-adjacent: @sensitive keeps them out of
/// debug output ([REDACTED] in DebugString/operator<<).
@sensitive
string ClientToken

enum CoffeeType {
    DRIP
    ESPRESSO
    CORTADO
    LATTE
}

/// A union member per milk preparation, to exercise union codegen.
union MilkOption {
    none: Unit
    dairy: DairyMilk
    alternative: AlternativeMilk
}

structure DairyMilk {
    @required
    percentFat: Float
}

structure AlternativeMilk {
    @required
    kind: String
}

union OrderStatus {
    pending: PendingStatus
    ready: ReadyStatus
    cancelled: CancelledStatus
}

structure PendingStatus {
    @required
    position: Integer
}

structure ReadyStatus {
    @required
    readyAt: Timestamp
}

structure CancelledStatus {
    reason: String
}

/// The order id does not correspond to any known order.
@error("client")
structure OrderNotFound {
    @required
    orderId: OrderId
}

/// The cafe cannot fulfill the order right now.
@error("server")
@retryable
structure OutOfBeans {
    message: String
}
