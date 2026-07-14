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
 * Pins the free-function collision guard (issue #47): a declared aggregate named {@code <Op>Error}
 * (any protocol) or {@code <Op>Response} (HTTP-binding servers) would make C++ name hiding disable
 * the serde functions inside the generated client/server, so generation rejects the model with the
 * shape, the helper, and the fix.
 */
class HelperNameCollisionTest {

  private static void generate(String modelText, String service) {
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
        operation Get { input := { detail: GetError } }

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
  void rejectsAResponseNamedShapeOnlyWhereTheHelperExists() {
    // Serialize<Op>Response is an HTTP-binding server helper; jsonRpc2 emits
    // Handle<Op> instead, so the same shape name is fine there.
    String model =
        """
        $version: "2.0"
        namespace test.collide
        use %s

        @%s
        service Svc { version: "1", operations: [Get] }
        @http(method: "POST", uri: "/get")
        operation Get { input := { payload: GetResponse } }

        structure GetResponse {
            message: String
        }
        """;
    CodegenException error =
        assertThrows(
            CodegenException.class,
            () ->
                generate(
                    model.formatted("alloy#simpleRestJson", "simpleRestJson"), "test.collide#Svc"));
    assertTrue(error.getMessage().contains("SerializeGetResponse"), error.getMessage());

    String rpcModel =
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
    assertDoesNotThrow(() -> generate(rpcModel, "test.collide#Svc"));
  }
}
