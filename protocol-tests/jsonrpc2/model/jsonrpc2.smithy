$version: "2.0"

namespace smithy.cpp.protocoltests.jsonrpc2

use smithy.cpp.protocols#jsonRpc2
use smithy.test#httpMalformedRequestTests
use smithy.test#httpRequestTests
use smithy.test#httpResponseTests

/// Authored conformance suite for smithy.cpp.protocols#jsonRpc2 (no external
/// suite exists — JSON-RPC 2.0 is an open standard but has no published
/// Smithy protocol tests). The cases pin the wire contract documented on the
/// trait: a single POST / endpoint, the request/response envelopes, the
/// constant client request id (1), server id echo (null before the envelope
/// yields one), and the JSON-RPC error-object mapping for modeled errors,
/// validation failures, and envelope-level failures.
@jsonRpc2
service JsonRpc2Protocol {
    version: "2026-01-01"
    operations: [
        EchoPayload
        NoArgs
        PutConstrained
    ]
}

@httpRequestTests([
    {
        id: "JsonRpc2BasicRequest"
        documentation: "Scalars serialize into params inside the request envelope; @jsonName renames the wire key."
        protocol: jsonRpc2
        method: "POST"
        uri: "/"
        headers: { "content-type": "application/json", "accept": "application/json" }
        body: """
            {"jsonrpc":"2.0","method":"EchoPayload","id":1,"params":{"string":"hello","renamed":"other","integer":42,"boolean":true,"double":3.5}}"""
        bodyMediaType: "application/json"
        params: {
            string: "hello"
            aliased: "other"
            integer: 42
            boolean: true
            double: 3.5
        }
    }
    {
        id: "JsonRpc2AggregatesRequest"
        documentation: "Lists, maps, and nested structures ride the shared JSON document pivot."
        protocol: jsonRpc2
        method: "POST"
        uri: "/"
        body: """
            {"jsonrpc":"2.0","method":"EchoPayload","id":1,"params":{"names":["a","b"],"attributes":{"k":"v"},"nested":{"label":"n","depth":2}}}"""
        bodyMediaType: "application/json"
        params: {
            names: ["a", "b"]
            attributes: { k: "v" }
            nested: { label: "n", depth: 2 }
        }
    }
    {
        id: "JsonRpc2TimestampsRequest"
        documentation: "Timestamps default to epoch-seconds; @timestampFormat(date-time) renders RFC3339."
        protocol: jsonRpc2
        method: "POST"
        uri: "/"
        body: """
            {"jsonrpc":"2.0","method":"EchoPayload","id":1,"params":{"timestamp":1515531081,"dateTime":"2018-01-09T20:51:21Z"}}"""
        bodyMediaType: "application/json"
        params: {
            timestamp: 1515531081
            dateTime: 1515531081
        }
    }
])
@httpResponseTests([
    {
        id: "JsonRpc2BasicResponse"
        documentation: "The output structure arrives as the envelope's result member."
        protocol: jsonRpc2
        code: 200
        headers: { "content-type": "application/json" }
        body: """
            {"jsonrpc":"2.0","result":{"echo":"hello","count":3,"nested":{"label":"n","depth":2}},"id":1}"""
        bodyMediaType: "application/json"
        params: {
            echo: "hello"
            count: 3
            nested: { label: "n", depth: 2 }
        }
    }
])
operation EchoPayload {
    input := {
        string: String

        @jsonName("renamed")
        aliased: String

        integer: Integer
        boolean: Boolean
        double: Double
        timestamp: Timestamp

        @timestampFormat("date-time")
        dateTime: Timestamp

        names: StringList
        attributes: StringMap
        nested: Nested
    }

    output := {
        echo: String
        count: Integer
        nested: Nested
    }

    errors: [NotFoundError, ThrottledError]
}

apply EchoPayload @httpMalformedRequestTests([
    {
        id: "JsonRpc2RejectsInvalidJson"
        documentation: "An unparseable body answers the reserved -32700 Parse error with a null id."
        protocol: jsonRpc2
        request: { method: "POST", uri: "/", body: "{", headers: { "content-type": "application/json" } }
        response: {
            code: 200
            headers: { "content-type": "application/json" }
            body: {
                mediaType: "application/json"
                assertion: {
                    contents: """
                        {"jsonrpc":"2.0","error":{"code":-32700,"message":"request body is not valid JSON","data":{"__type":"SerializationException"}},"id":null}"""
                }
            }
        }
    }
    {
        id: "JsonRpc2RejectsNonObjectEnvelope"
        documentation: "A JSON body that is not an object is an invalid request (-32600)."
        protocol: jsonRpc2
        request: { method: "POST", uri: "/", body: "[1,2,3]", headers: { "content-type": "application/json" } }
        response: {
            code: 200
            headers: { "content-type": "application/json" }
            body: {
                mediaType: "application/json"
                assertion: {
                    contents: """
                        {"jsonrpc":"2.0","error":{"code":-32600,"message":"request is not a JSON-RPC 2.0 call","data":{"__type":"SerializationException"}},"id":null}"""
                }
            }
        }
    }
    {
        id: "JsonRpc2RejectsWrongVersion"
        documentation: "jsonrpc must be the string \"2.0\"; the id is still echoed once parsed."
        protocol: jsonRpc2
        request: {
            method: "POST"
            uri: "/"
            body: """
                {"jsonrpc":"1.0","method":"EchoPayload","id":7}"""
            headers: { "content-type": "application/json" }
        }
        response: {
            code: 200
            headers: { "content-type": "application/json" }
            body: {
                mediaType: "application/json"
                assertion: {
                    contents: """
                        {"jsonrpc":"2.0","error":{"code":-32600,"message":"expected jsonrpc: \\"2.0\\"","data":{"__type":"SerializationException"}},"id":7}"""
                }
            }
        }
    }
    {
        id: "JsonRpc2RejectsMissingMethod"
        documentation: "A call without a string method member is an invalid request (-32600)."
        protocol: jsonRpc2
        request: {
            method: "POST"
            uri: "/"
            body: """
                {"jsonrpc":"2.0","id":3}"""
            headers: { "content-type": "application/json" }
        }
        response: {
            code: 200
            headers: { "content-type": "application/json" }
            body: {
                mediaType: "application/json"
                assertion: {
                    contents: """
                        {"jsonrpc":"2.0","error":{"code":-32600,"message":"expected a string method member","data":{"__type":"SerializationException"}},"id":3}"""
                }
            }
        }
    }
    {
        id: "JsonRpc2RejectsUnknownMethod"
        documentation: "A method naming no operation answers -32601 Method not found."
        protocol: jsonRpc2
        request: {
            method: "POST"
            uri: "/"
            body: """
                {"jsonrpc":"2.0","method":"DoesNotExist","id":4}"""
            headers: { "content-type": "application/json" }
        }
        response: {
            code: 200
            headers: { "content-type": "application/json" }
            body: {
                mediaType: "application/json"
                assertion: {
                    contents: """
                        {"jsonrpc":"2.0","error":{"code":-32601,"message":"unknown method: DoesNotExist","data":{"__type":"UnknownOperationException"}},"id":4}"""
                }
            }
        }
    }
    {
        id: "JsonRpc2RejectsNonObjectParams"
        documentation: "params must be an object when present (-32602 Invalid params)."
        protocol: jsonRpc2
        request: {
            method: "POST"
            uri: "/"
            body: """
                {"jsonrpc":"2.0","method":"EchoPayload","params":5,"id":6}"""
            headers: { "content-type": "application/json" }
        }
        response: {
            code: 200
            headers: { "content-type": "application/json" }
            body: {
                mediaType: "application/json"
                assertion: {
                    contents: """
                        {"jsonrpc":"2.0","error":{"code":-32602,"message":"params must be an object","data":{"__type":"SerializationException"}},"id":6}"""
                }
            }
        }
    }
    {
        id: "JsonRpc2RejectsWrongContentType"
        documentation: "A present Content-Type must carry application/json."
        protocol: jsonRpc2
        request: {
            method: "POST"
            uri: "/"
            body: """
                {"jsonrpc":"2.0","method":"EchoPayload","id":1}"""
            headers: { "content-type": "text/plain" }
        }
        response: {
            code: 200
            headers: { "content-type": "application/json" }
            body: {
                mediaType: "application/json"
                assertion: {
                    contents: """
                        {"jsonrpc":"2.0","error":{"code":-32600,"message":"expected content-type: application/json","data":{"__type":"UnsupportedMediaTypeException"}},"id":null}"""
                }
            }
        }
    }
])

@httpRequestTests([
    {
        id: "JsonRpc2NoParamsRequest"
        documentation: "Operations with no modeled input omit the params member entirely."
        protocol: jsonRpc2
        method: "POST"
        uri: "/"
        headers: { "content-type": "application/json", "accept": "application/json" }
        body: """
            {"jsonrpc":"2.0","method":"NoArgs","id":1}"""
        bodyMediaType: "application/json"
        params: {}
    }
])
@httpResponseTests([
    {
        id: "JsonRpc2EmptyResponse"
        documentation: "An operation with no modeled output answers an empty result object."
        protocol: jsonRpc2
        code: 200
        headers: { "content-type": "application/json" }
        body: """
            {"jsonrpc":"2.0","result":{},"id":1}"""
        bodyMediaType: "application/json"
        params: {}
    }
])
operation NoArgs {}

@httpRequestTests([
    {
        id: "JsonRpc2ConstrainedRequest"
        documentation: "A valid constrained input passes validation and reaches the handler."
        protocol: jsonRpc2
        method: "POST"
        uri: "/"
        body: """
            {"jsonrpc":"2.0","method":"PutConstrained","id":1,"params":{"name":"ok","limit":10}}"""
        bodyMediaType: "application/json"
        params: { name: "ok", limit: 10 }
    }
])
operation PutConstrained {
    input := {
        @required
        @length(min: 1, max: 8)
        name: String

        @range(min: 1, max: 100)
        limit: Integer

        @pattern("^[a-z0-9-]+$")
        slug: String

        // Catastrophic under a backtracking engine (ReDoS); the generated
        // validator must evaluate it in linear time.
        @pattern("^([0-9]+)+$")
        evilDigits: String
    }

    output := {
        @required
        accepted: Boolean
    }
}

apply PutConstrained @httpMalformedRequestTests([
    {
        id: "JsonRpc2ValidationFailure"
        documentation: "Constraint violations answer a 400-coded error envelope carrying smithy.framework#ValidationException identity and the fieldList."
        protocol: jsonRpc2
        request: {
            method: "POST"
            uri: "/"
            body: """
                {"jsonrpc":"2.0","method":"PutConstrained","params":{"name":""},"id":9}"""
            headers: { "content-type": "application/json" }
        }
        response: {
            code: 200
            headers: { "content-type": "application/json" }
            body: {
                mediaType: "application/json"
                assertion: {
                    contents: """
                        {"jsonrpc":"2.0","error":{"code":400,"message":"1 validation error detected. Value with length 0 at '/name' failed to satisfy constraint: Member must have length between 1 and 8, inclusive","data":{"__type":"smithy.framework#ValidationException","fieldList":[{"message":"Value with length 0 at '/name' failed to satisfy constraint: Member must have length between 1 and 8, inclusive","path":"/name"}]}},"id":9}"""
                }
            }
        }
    }
    {
        id: "JsonRpc2PatternMismatch"
        documentation: "@pattern violations report the suite-exact message with the pattern text."
        protocol: jsonRpc2
        request: {
            method: "POST"
            uri: "/"
            body: """
                {"jsonrpc":"2.0","method":"PutConstrained","params":{"name":"ok","slug":"Not Valid!"},"id":10}"""
            headers: { "content-type": "application/json" }
        }
        response: {
            code: 200
            headers: { "content-type": "application/json" }
            body: {
                mediaType: "application/json"
                assertion: {
                    contents: """
                        {"jsonrpc":"2.0","error":{"code":400,"message":"1 validation error detected. Value at '/slug' failed to satisfy constraint: Member must satisfy regular expression pattern: ^[a-z0-9-]+$","data":{"__type":"smithy.framework#ValidationException","fieldList":[{"message":"Value at '/slug' failed to satisfy constraint: Member must satisfy regular expression pattern: ^[a-z0-9-]+$","path":"/slug"}]}},"id":10}"""
                }
            }
        }
    }
    {
        id: "JsonRpc2PatternReDoSInput"
        documentation: "When the pattern is susceptible to catastrophic backtracking, the server answers promptly instead of hanging while evaluating it (linear-time engine)."
        protocol: jsonRpc2
        request: {
            method: "POST"
            uri: "/"
            body: """
                {"jsonrpc":"2.0","method":"PutConstrained","params":{"name":"ok","evilDigits":"00000000000000000000000000000000000000000000000000!"},"id":11}"""
            headers: { "content-type": "application/json" }
        }
        response: {
            code: 200
            headers: { "content-type": "application/json" }
            body: {
                mediaType: "application/json"
                assertion: {
                    contents: """
                        {"jsonrpc":"2.0","error":{"code":400,"message":"1 validation error detected. Value at '/evilDigits' failed to satisfy constraint: Member must satisfy regular expression pattern: ^([0-9]+)+$","data":{"__type":"smithy.framework#ValidationException","fieldList":[{"message":"Value at '/evilDigits' failed to satisfy constraint: Member must satisfy regular expression pattern: ^([0-9]+)+$","path":"/evilDigits"}]}},"id":11}"""
                }
            }
        }
    }
])

@error("client")
@httpError(404)
@httpResponseTests([
    {
        id: "JsonRpc2NotFoundError"
        documentation: "Modeled errors ride the JSON-RPC error object: code from @httpError, data carries the members plus the fully qualified shape id in __type, message mirrors the detail's message."
        protocol: jsonRpc2
        code: 200
        headers: { "content-type": "application/json" }
        body: """
            {"jsonrpc":"2.0","error":{"code":404,"message":"no such sink","data":{"__type":"smithy.cpp.protocoltests.jsonrpc2#NotFoundError","message":"no such sink","resourceType":"Sink"}},"id":1}"""
        bodyMediaType: "application/json"
        params: { message: "no such sink", resourceType: "Sink" }
    }
])
structure NotFoundError {
    @required
    message: String

    resourceType: String
}

@error("server")
@httpError(503)
@retryable
@httpResponseTests([
    {
        id: "JsonRpc2ThrottledError"
        documentation: "Server-class errors carry their 5xx @httpError status as the JSON-RPC code, marking them retryable client-side."
        protocol: jsonRpc2
        code: 200
        headers: { "content-type": "application/json" }
        body: """
            {"jsonrpc":"2.0","error":{"code":503,"message":"slow down","data":{"__type":"smithy.cpp.protocoltests.jsonrpc2#ThrottledError","message":"slow down"}},"id":1}"""
        bodyMediaType: "application/json"
        params: { message: "slow down" }
    }
])
structure ThrottledError {
    message: String
}

structure Nested {
    @required
    label: String

    depth: Integer
}

list StringList {
    member: String
}

map StringMap {
    key: String
    value: String
}
