package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.codegen.core.CodegenException;

/**
 * Pins issue #71's guard: a shape whose declared C++ type name matches a generated file-local
 * helper fails generation with the attributed cpp-codegen: diagnostic instead of a raw C++ error.
 * One rejection per name family (client error support, Parse&lt;Op&gt;Error, Make&lt;X&gt;Error,
 * ErrorToResponse, the protocol's error envelope, Handle&lt;Op&gt;, Build&lt;Op&gt;Response,
 * Validate&lt;X&gt;), plus accepts-cases pinning that mode, emission, and protocol scoping don't
 * reserve names the run's generated files never declare.
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

  /** rpcv2Cbor service whose Get input references {@code shapeName}. */
  private static String cborModel(String shapeName) {
    return """
        $version: "2.0"
        namespace test.reserved
        use smithy.protocols#rpcv2Cbor

        @rpcv2Cbor
        service Svc { version: "1", operations: [Get] }
        operation Get {
            input := { name: String, payload: %s }
        }

        structure %s {
            value: String
        }
        """
        .formatted(shapeName, shapeName);
  }

  private static void rejects(String model, String mode, String... fragments) {
    CodegenException error =
        assertThrows(
            CodegenException.class,
            () ->
                PluginTestHarness.generate(
                    model, "test.reserved#Svc", "test::reserved", b -> b.withMember("mode", mode)));
    for (String fragment : fragments) {
      assertTrue(error.getMessage().contains(fragment), error.getMessage());
    }
  }

  private static void accepts(String model, String mode) {
    PluginTestHarness.generate(
        model, "test.reserved#Svc", "test::reserved", b -> b.withMember("mode", mode));
  }

  @Test
  void clientErrorSupportHelperNameIsRejected() {
    // The issue's least contrived trigger: GenericError is a plausible model
    // name and a fixed client.cc helper.
    rejects(
        jsonRpcModel("GenericError", true),
        "client",
        "cpp-codegen: shape test.reserved#GenericError",
        "GenericError helper");
  }

  @Test
  void clientPerOperationErrorParserNameIsRejected() {
    rejects(
        jsonRpcModel("ParseGetError", true), "client", "ParseGetError helper (test.reserved#Get)");
  }

  @Test
  void clientErrorFactoryNameIsRejected() {
    rejects(
        jsonRpcModel("MakeKickedError", true),
        "client",
        "MakeKickedError helper (test.reserved#Kicked)");
  }

  @Test
  void serverErrorMapperNameIsRejected() {
    rejects(jsonRpcModel("ErrorToResponse", true), "server", "shape test.reserved#ErrorToResponse");
  }

  @Test
  void serverErrorEnvelopeNameIsRejected() {
    // The envelope name comes from the protocol's ErrorResponseSpec.
    rejects(jsonRpcModel("JsonRpcError", true), "server", "JsonRpcError helper");
  }

  @Test
  void rpcDispatchHelperNameIsRejected() {
    rejects(jsonRpcModel("HandleGet", true), "server", "HandleGet helper (test.reserved#Get)");
  }

  @Test
  void httpResponseBuilderNameIsRejected() {
    rejects(restModel("BuildGetResponse"), "server", "BuildGetResponse helper (test.reserved#Get)");
  }

  @Test
  void numericParseHelperNameIsRejected() {
    rejects(restModel("ParseInt64Text"), "client", "ParseInt64Text helper");
  }

  @Test
  void validationWiringHelperNameIsRejected() {
    // Get's constrained input makes the wiring real for this run.
    rejects(restModel("ValidationErrorResponse"), "server", "ValidationErrorResponse helper");
  }

  @Test
  void validatorNameIsRejected() {
    // Get's input carries an @length constraint, so the server declares
    // ValidateGetInput; a shape declaring that C++ type name collides.
    rejects(restModel("ValidateGetInput"), "server", "ValidateGetInput helper");
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
    // Handle<Op> is jsonRpc2 dispatch; an HTTP-binding server never declares it.
    accepts(restModel("HandleGet"), "server");
  }

  @Test
  void inlinedDispatchDoesNotReserveHandleNames() {
    // rpcv2Cbor inlines dispatch into its route lambdas — no Handle<Op> exists.
    accepts(cborModel("HandleGet"), "server");
  }

  @Test
  void numericParseHelpersAreHttpBindingOnly() {
    // The RPC protocols decode numbers from the body document and never
    // declare the text-to-number helpers.
    accepts(jsonRpcModel("ParseInt64Text", true), "client");
    accepts(cborModel("ParseDoubleText"), "server");
  }

  @Test
  void validationWiringNeedsValidators() {
    // No constrained input and no HTTP top-level @required: the wiring (and
    // its names) never exists for this run.
    accepts(jsonRpcModel("ValidationErrorResponse", true), "server");
  }
}
