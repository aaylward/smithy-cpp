$version: "2.0"

namespace smithy.cpp.protocols

/// A vendor-neutral JSON-RPC 2.0 protocol. A single POST endpoint carries
/// {"jsonrpc":"2.0","method":<operation>,"params":<input>,"id":…}; responses
/// carry {"jsonrpc":"2.0","result":<output>,"id":…} or a JSON-RPC error
/// object mapped from the modeled error. No HTTP bindings apply.
///
/// The REST/JSON protocol is alloy#simpleRestJson (adopted from alloy-core);
/// the RPC/CBOR protocol is smithy.protocols#rpcv2Cbor. This trait covers the
/// JSON-RPC option, which has no equivalent in either.
@trait(selector: "service")
@protocolDefinition(traits: [
    smithy.api#jsonName
    smithy.api#timestampFormat
])
structure jsonRpc2 {}
