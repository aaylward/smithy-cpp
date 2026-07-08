// Protocol binding overlay: pairs with model/unions.smithy to bind the
// protocol-agnostic UnionGauntlet service to JSON-RPC 2.0.
$version: "2.0"

namespace compile.unions

use smithy.cpp.protocols#jsonRpc2

apply UnionGauntlet @jsonRpc2
