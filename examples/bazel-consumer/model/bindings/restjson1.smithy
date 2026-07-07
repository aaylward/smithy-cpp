// Protocol binding overlay: pairs with model/todo.smithy to bind the
// protocol-agnostic Todo service to restJson1. Pass both files to the
// generation rule; the base model never mentions a protocol.
$version: "2.0"

namespace acme.todo

use aws.protocols#restJson1

apply Todo @restJson1
