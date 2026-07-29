package io.smithycpp.codegen;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.neighbor.Walker;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;

/**
 * Rejects model shapes whose declared C++ type name matches a helper the generated client/server
 * declares in its anonymous namespace (issue #71). Such a shape compiles into a type declaration
 * that C++ name lookup resolves against the same-named file-local helper, so generation would
 * produce a raw C++ error deep inside client.cc/server.cc; this guard fails generation up front
 * with the shape, the helper, and the fix instead.
 *
 * <p>Only names of helpers this run actually emits are reserved — every reservation is a name a
 * model may no longer use. Each name comes from the code that emits it (ProtocolSupport's helper
 * constants and name derivations, the protocol's helper-name plug points, ValidationGenerator's
 * wiring and validator names), so a helper rename or a new emission condition moves its reservation
 * with it. Streaming-only helper names (DialStream, Serve&lt;Op&gt;Async,
 * Encode/Decode&lt;Op&gt;Event, ...) stay unreserved deliberately: a collision there is contrived.
 */
final class ReservedHelperNames {

  private ReservedHelperNames() {}

  static void reject(
      CppContext context,
      ProtocolGenerator protocol,
      ServiceShape service,
      List<OperationShape> operations) {
    CppSettings settings = context.settings();
    Map<String, String> reserved = new HashMap<>();

    if (settings.generateClient()) {
      for (String name : ProtocolSupport.CLIENT_ERROR_HELPERS) {
        reserve(reserved, "client", name, "");
      }
      if (protocol.usesNumericParseHelpers()) {
        for (String name : ProtocolSupport.NUMERIC_PARSE_HELPERS) {
          reserve(reserved, "client", name, "");
        }
      }
      for (OperationShape operation : operations) {
        List<ShapeId> errors = operation.getErrors(service);
        // Parse<Op>Error exists only for unary operations that declare errors;
        // error-less ones return GenericError, streaming ones report on the
        // stream (writeOperationErrorParsers' skip logic).
        if (!errors.isEmpty() && !EventStreamCodeGen.streaming(context.model(), operation)) {
          reserve(
              reserved,
              "client",
              ProtocolSupport.parseErrorFunction(operation),
              " (" + operation.getId() + ")");
        }
        for (ShapeId errorId : errors) {
          StructureShape errorShape =
              context.model().expectShape(errorId).asStructureShape().orElseThrow();
          reserve(
              reserved,
              "client",
              ProtocolSupport.makeErrorFunction(context, errorShape),
              " (" + errorId + ")");
        }
      }
    }

    if (settings.generateServer()) {
      reserve(reserved, "server", ProtocolSupport.ERROR_TO_RESPONSE, "");
      reserve(reserved, "server", protocol.errorResponseSpec().errorFn(), "");
      if (protocol.usesNumericParseHelpers()) {
        for (String name : ProtocolSupport.NUMERIC_PARSE_HELPERS) {
          reserve(reserved, "server", name, "");
        }
      }
      for (OperationShape operation : operations) {
        String opName = CppReservedWords.escape(operation.getId().getName());
        boolean streaming = EventStreamCodeGen.streaming(context.model(), operation);
        for (String helper : protocol.serverOperationHelperNames(opName, streaming)) {
          reserve(reserved, "server", helper, " (" + operation.getId() + ")");
        }
      }
      ValidationGenerator validation = new ValidationGenerator(context, operations);
      if (validation.wiringNeeded(protocol.validationWiringAlsoEmitted(context, operations))) {
        for (String name : ValidationGenerator.WIRING_HELPERS) {
          reserve(reserved, "server", name, "");
        }
      }
      for (String validator : validation.validatorNames()) {
        reserve(reserved, "server", validator, "");
      }
    }

    for (Shape shape : new Walker(context.model()).walkShapes(service)) {
      if (!CppSymbolProvider.declaresType(shape)) {
        continue;
      }
      String declared = context.cppSymbols().declaredName(shape);
      String helper = reserved.get(declared);
      if (helper != null) {
        throw new CodegenException(
            "cpp-codegen: shape "
                + shape.getId()
                + " declares C++ type "
                + declared
                + ", which collides with "
                + helper
                + " declared in the same generated file — C++ name lookup would hide one behind"
                + " the other; rename the shape");
      }
    }
  }

  /** First reservation wins: one name can back several helpers, the diagnostic names one. */
  private static void reserve(
      Map<String, String> reserved, String side, String name, String attribution) {
    reserved.putIfAbsent(name, "the generated " + side + "'s " + name + " helper" + attribution);
  }
}
