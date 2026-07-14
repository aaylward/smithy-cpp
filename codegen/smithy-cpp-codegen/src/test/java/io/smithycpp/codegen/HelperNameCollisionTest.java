package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;
import software.amazon.smithy.build.PluginContext;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.node.Node;

/**
 * Pins the free-function collision guard (issue #47): a serde-carrying shape named {@code
 * <Op>Error} (any protocol) or {@code <Op>Response} (HTTP-binding servers) would make C++ name
 * hiding disable the serde functions inside the generated client/server, so generation rejects the
 * model with the shape, the helper, and the fix. Only names of helpers the run actually emits are
 * reserved — the accepts* tests pin each dimension of that scoping.
 */
class HelperNameCollisionTest {

  private static void generate(String modelText, String service) {
    generate(modelText, service, "both");
  }

  private static void generate(String modelText, String service, String mode) {
    Model model =
        Model.assembler()
            .discoverModels(HelperNameCollisionTest.class.getClassLoader())
            .addUnparsedModel("collision.smithy", modelText)
            .assemble()
            .unwrap();
    new CppCodegenPlugin()
        .execute(
            PluginContext.builder()
                .fileManifest(new MockManifest())
                .model(model)
                .settings(
                    Node.objectNodeBuilder()
                        .withMember("service", service)
                        .withMember("namespace", "test::collide")
                        .withMember("mode", mode)
                        .build())
                .build());
  }

  @Test
  void rejectsAShapeNamedAfterTheOperationErrorHelper() {
    String model =
        """
        $version: "2.0"
        namespace test.collide
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Get] }
        operation Get {
            input := { detail: GetError }
            errors: [Oops]
        }

        @error("client")
        structure Oops {
            message: String
        }

        structure GetError {
            message: String
        }
        """;
    CodegenException error =
        assertThrows(CodegenException.class, () -> generate(model, "test.collide#Svc"));
    assertTrue(error.getMessage().contains("test.collide#GetError"), error.getMessage());
    assertTrue(error.getMessage().contains("DeserializeGetError"), error.getMessage());
    assertTrue(error.getMessage().contains("rename"), error.getMessage());
  }

  @Test
  void acceptsAnErrorNamedShapeWhenTheOperationDeclaresNoErrors() {
    // Deserialize<Op>Error only exists for operations with declared errors;
    // an error-less Get never emits it, so structure GetError collides with
    // nothing and the model must generate.
    String model =
        """
        $version: "2.0"
        namespace test.collide
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Get] }
        operation Get { input := { detail: GetError } }

        structure GetError {
            message: String
        }
        """;
    assertDoesNotThrow(() -> generate(model, "test.collide#Svc"));
  }

  @Test
  void acceptsAnEnumNamedAfterTheErrorHelper() {
    // Enums convert through FromString/ToString, not Serialize/Deserialize
    // functions, so an enum's name cannot hide any helper.
    String model =
        """
        $version: "2.0"
        namespace test.collide
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Get] }
        operation Get {
            input := { detail: GetError }
            errors: [Oops]
        }

        @error("client")
        structure Oops {
            message: String
        }

        enum GetError {
            SOMETHING
        }
        """;
    assertDoesNotThrow(() -> generate(model, "test.collide#Svc"));
  }

  private static final String RESPONSE_COLLISION_HTTP_MODEL =
      """
      $version: "2.0"
      namespace test.collide
      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Get] }
      @http(method: "POST", uri: "/get")
      operation Get { input := { payload: GetResponse } }

      structure GetResponse {
          message: String
      }
      """;

  @Test
  void rejectsAResponseNamedShapeForHttpBindingServers() {
    CodegenException error =
        assertThrows(
            CodegenException.class,
            () -> generate(RESPONSE_COLLISION_HTTP_MODEL, "test.collide#Svc"));
    assertTrue(error.getMessage().contains("SerializeGetResponse"), error.getMessage());
  }

  @Test
  void acceptsAResponseNamedShapeWhenOnlyTheClientIsGenerated() {
    // Serialize<Op>Response lives in server.cc; mode=client never emits that
    // file, so the same model must generate (it did before the guard existed).
    assertDoesNotThrow(() -> generate(RESPONSE_COLLISION_HTTP_MODEL, "test.collide#Svc", "client"));
  }

  @Test
  void acceptsAResponseNamedShapeForRpcProtocols() {
    // jsonRpc2 emits Handle<Op> instead of Serialize<Op>Response, so the same
    // shape name is fine there.
    String model =
        """
        $version: "2.0"
        namespace test.collide
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Get] }
        operation Get { input := { payload: GetResponse } }

        structure GetResponse {
            message: String
        }
        """;
    assertDoesNotThrow(() -> generate(model, "test.collide#Svc"));
  }
}
