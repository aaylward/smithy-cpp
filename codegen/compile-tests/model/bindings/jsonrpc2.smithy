// Protocol binding overlay: pairs with model/gauntlet.smithy to bind the
// protocol-agnostic Gauntlet service to JSON-RPC 2.0. The @http traits in the
// base model are simply ignored by this protocol.
$version: "2.0"

namespace compile.gauntlet

use smithy.cpp.protocols#jsonRpc2

apply Gauntlet @jsonRpc2
