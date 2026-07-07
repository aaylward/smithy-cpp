// Protocol binding overlay: pairs with model/todo.smithy to bind the same
// protocol-agnostic Todo service to rpcv2Cbor. The @http traits in the base
// model are simply ignored by this protocol.
$version: "2.0"

namespace acme.todo

use smithy.protocols#rpcv2Cbor

apply Todo @rpcv2Cbor
