package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.codegen.core.CodegenException;

/**
 * Pins issue #71's guard: a shape whose declared C++ type name matches a generated file-local
 * helper fails generation with the attributed cpp-codegen: diagnostic instead of a raw C++ error.
 * One rejection per name family (client fixed, Parse&lt;Op&gt;Error, Make&lt;X&gt;Error, server
 * fixed, Handle&lt;Op&gt;, Build&lt;Op&gt;Response, Validate&lt;X&gt;), plus accepts-cases pinning
 * that mode, emission, and protocol scoping don't over-reject.
 */
class ReservedHelperNamesTest {

  /** jsonRpc2 service whose Get input references {@code shapeName}, with or without errors. */
  private static String jsonRpcModel(String shapeName, boolean withErrors) {
    return """
        $version: "2.0"
        namespace test.reserved
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Get] }
        operation Get {
            input := { name: String, payload: %s }
            %s
        }

        @error("client")
        structure Kicked {
            message: String
        }

        structure %s {
            value: String
        }
        """
        .formatted(shapeName, withErrors ? "errors: [Kicked]" : "", shapeName);
  }

  /**
   * simpleRestJson service with a constrained input member and a reference to {@code shapeName}.
   */
  private static String restModel(String shapeName) {
    return """
        $version: "2.0"
        namespace test.reserved
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Get] }
        @http(method: "POST", uri: "/get")
        operation Get {
            input := {
                @length(min: 1)
                name: String

                payload: %s
            }
        }

        structure %s {
            value: String
        }
        """
        .formatted(shapeName, shapeName);
  }

  private static CodegenException rejects(String model, String mode) {
    return assertThrows(
        CodegenException.class,
        () ->
            PluginTestHarness.generate(
                model, "test.reserved#Svc", "test::reserved", b -> b.withMember("mode", mode)));
  }

  private static void accepts(String model, String mode) {
    PluginTestHarness.generate(
        model, "test.reserved#Svc", "test::reserved", b -> b.withMember("mode", mode));
  }

  @Test
  void clientFixedHelperNameIsRejected() {
    // The issue's least contrived trigger: GenericError is a plausible model
    // name and a fixed client.cc helper.
    CodegenException error = rejects(jsonRpcModel("GenericError", true), "client");
    assertTrue(
        error.getMessage().contains("cpp-codegen: shape test.reserved#GenericError"),
        error.getMessage());
    assertTrue(error.getMessage().contains("GenericError helper"), error.getMessage());
  }

  @Test
  void clientPerOperationErrorParserNameIsRejected() {
    CodegenException error = rejects(jsonRpcModel("ParseGetError", true), "client");
    assertTrue(
        error.getMessage().contains("ParseGetError helper (test.reserved#Get)"),
        error.getMessage());
  }

  @Test
  void clientErrorFactoryNameIsRejected() {
    CodegenException error = rejects(jsonRpcModel("MakeKickedError", true), "client");
    assertTrue(
        error.getMessage().contains("MakeKickedError helper (test.reserved#Kicked)"),
        error.getMessage());
  }

  @Test
  void serverFixedHelperNameIsRejected() {
    CodegenException error = rejects(jsonRpcModel("ValidationErrorResponse", true), "server");
    assertTrue(
        error.getMessage().contains("shape test.reserved#ValidationErrorResponse"),
        error.getMessage());
  }

  @Test
  void rpcDispatchHelperNameIsRejected() {
    CodegenException error = rejects(jsonRpcModel("HandleGet", true), "server");
    assertTrue(
        error.getMessage().contains("HandleGet helper (test.reserved#Get)"), error.getMessage());
  }

  @Test
  void httpResponseBuilderNameIsRejected() {
    CodegenException error = rejects(restModel("BuildGetResponse"), "server");
    assertTrue(
        error.getMessage().contains("BuildGetResponse helper (test.reserved#Get)"),
        error.getMessage());
  }

  @Test
  void validatorNameIsRejected() {
    // Get's input carries an @length constraint, so the server declares
    // ValidateGetInput; a shape declaring that C++ type name collides.
    CodegenException error = rejects(restModel("ValidateGetInput"), "server");
    assertTrue(
        error.getMessage().contains("ValidateGetInput validation helper"), error.getMessage());
  }

  @Test
  void modeScopingDoesNotOverReject() {
    // Client-only fixed names stay free for a server-only generation run: the
    // server file never declares SanitizeErrorCode.
    accepts(jsonRpcModel("SanitizeErrorCode", true), "server");
  }

  @Test
  void emissionScopingDoesNotOverReject() {
    // Parse<Op>Error only exists for operations that declare errors; with no
    // errors on Get the name is free.
    accepts(jsonRpcModel("ParseGetError", false), "client");
  }

  @Test
  void protocolScopingDoesNotOverReject() {
    // Handle<Op> is RPC dispatch; an HTTP-binding server never declares it.
    accepts(restModel("HandleGet"), "server");
  }
}
