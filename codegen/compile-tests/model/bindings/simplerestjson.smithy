// Protocol binding overlay: pairs with model/gauntlet.smithy to bind the
// protocol-agnostic Gauntlet service to simpleRestJson.
$version: "2.0"

namespace compile.gauntlet

use alloy#simpleRestJson

apply Gauntlet @simpleRestJson
