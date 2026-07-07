// Protocol binding overlay for greeter.smithy: the base model stays
// protocol-agnostic; passing both files to a rule binds simpleRestJson.
$version: "2.0"

namespace smithy.cpp.ruletest

use alloy#simpleRestJson

apply Greeter @simpleRestJson
