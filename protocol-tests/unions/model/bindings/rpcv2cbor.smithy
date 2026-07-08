// Protocol binding overlay: pairs with model/unions.smithy to bind the
// protocol-agnostic UnionGauntlet service to rpcv2Cbor.
$version: "2.0"

namespace compile.unions

use smithy.protocols#rpcv2Cbor

apply UnionGauntlet @rpcv2Cbor
