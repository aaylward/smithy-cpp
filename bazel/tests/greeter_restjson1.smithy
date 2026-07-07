// Protocol binding overlay for greeter.smithy: the base model stays
// protocol-agnostic; passing both files to a rule binds restJson1.
$version: "2.0"

namespace smithy.cpp.ruletest

use aws.protocols#restJson1

apply Greeter @restJson1
