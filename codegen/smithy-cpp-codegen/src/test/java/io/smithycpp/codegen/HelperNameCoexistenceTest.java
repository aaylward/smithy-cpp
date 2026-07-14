package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;

/**
 * Pins the resolution of issue #64's last item: per-operation helpers stay out of the serde
 * functions' Serialize/Deserialize&lt;Shape&gt; naming pattern (Parse&lt;Op&gt;Error,
 * Build&lt;Op&gt;Response), so a shape named after an operation coexists with the helpers instead
 * of being hidden by them (C++ name hiding) — models #69's guard used to reject now generate, and
 * the guard is gone. Each test uses the model shape that actually materialized the hiding: a
 * same-named serde call inside the file that declares the helper.
 */
class HelperNameCoexistenceTest {

  @Test
  void errorShapeNamedAfterTheOperationCoexistsWithTheErrorHelper() {
    // Make<Shape>Error deserializes the error detail INSIDE client.cc, so an
    // error shape named GetError calls serde's DeserializeGetError(Document)
    // in the same file that declares the per-operation error helper. With the
    // helper named DeserializeGetError(HttpResponse) that call was hidden;
    // as ParseGetError the two coexist.
    String model =
        """
        $version: "2.0"
        namespace test.coexist
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Get] }
        operation Get {
            input := { name: String }
            errors: [GetError]
        }

        @error("client")
        structure GetError {
            message: String
        }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.coexist#Svc", "test::coexist");
    String client = manifest.expectFileString("/src/client.cc");
    assertTrue(
        client.contains("smithy::Error ParseGetError(const smithy::http::HttpResponse&"), client);
    assertTrue(client.contains("DeserializeGetError(parsed.doc)"), client);
  }

  @Test
  void outputMemberTypeNamedAfterTheOperationCoexistsWithTheResponseHelper() {
    // Build<Op>Response serializes the output body INSIDE server.cc, so an
    // output member of type GetResponse calls serde's SerializeGetResponse in
    // the same file. A helper named SerializeGetResponse hid that call from
    // within its own body; as BuildGetResponse the two coexist.
    String model =
        """
        $version: "2.0"
        namespace test.coexist
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Get] }
        @http(method: "POST", uri: "/get")
        operation Get {
            input := { name: String }
            output := { payload: GetResponse }
        }

        structure GetResponse {
            message: String
        }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.coexist#Svc", "test::coexist");
    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(server.contains("smithy::http::HttpResponse BuildGetResponse("), server);
    // The full call expression, not just the name — a regressed helper
    // declaration would also contain "SerializeGetResponse(".
    assertTrue(server.contains("SerializeGetResponse((*output.payload))"), server);
  }

  @Test
  void errorLessOperationsSkipTheErrorParserAndFallBackToGenericError() {
    // Parse<Op>Error only exists for operations that declare errors; an
    // error-less operation's body returns GenericError(ParseError(...))
    // directly (previously pinned by a guard-scoping test, now by output).
    String model =
        """
        $version: "2.0"
        namespace test.coexist
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Get] }
        operation Get { input := { name: String } }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.coexist#Svc", "test::coexist");
    String client = manifest.expectFileString("/src/client.cc");
    assertFalse(client.contains("ParseGetError"), client);
    assertTrue(client.contains("GenericError(ParseError(*response))"), client);
  }
}
