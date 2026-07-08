// Protocol binding overlay: pairs with model/gauntlet.smithy to bind the
// protocol-agnostic Gauntlet service to rpcv2Cbor. The @http traits in the
// base model are simply ignored by this protocol.
$version: "2.0"

namespace compile.gauntlet

use smithy.protocols#rpcv2Cbor

apply Gauntlet @rpcv2Cbor
