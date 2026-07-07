// Protocol binding overlay: pairs with model/todo.smithy to bind the same
// protocol-agnostic Todo service to JSON-RPC 2.0. The @http traits in the
// base model are simply ignored by this protocol.
$version: "2.0"

namespace acme.todo

use smithy.cpp.protocols#jsonRpc2

apply Todo @jsonRpc2
