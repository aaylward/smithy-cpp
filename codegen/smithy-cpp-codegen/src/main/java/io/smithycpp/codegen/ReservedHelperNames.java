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

/**
 * Rejects model shapes whose declared C++ type name matches a helper the generated client/server
 * declares in its anonymous namespace (issue #71). Such a shape compiles into a type declaration
 * that C++ name lookup resolves against the same-named file-local helper, so generation would
 * produce a raw C++ error deep inside client.cc/server.cc; this guard fails generation up front
 * with the shape, the helper, and the fix instead.
 *
 * <p>Only names of helpers this run actually emits are reserved, mirroring the mode and emission
 * scoping the deleted #69 guard learned: client-only names need {@code generateClient()}, {@code
 * Parse<Op>Error} needs the operation to declare errors, RPC protocols reserve {@code Handle<Op>}
 * where HTTP binding reserves {@code Parse<Op>Input}/{@code Build<Op>Response}, and validators are
 * reserved from the exact constrained-shape closure ValidationGenerator will emit. Streaming-only
 * helper names (DialStream, Serve&lt;Op&gt;Async, Encode/Decode&lt;Op&gt;Event, ...) stay
 * unreserved deliberately: a collision there is contrived, and every reservation is a name a model
 * may no longer use.
 */
final class ReservedHelperNames {

  private ReservedHelperNames() {}

  /** Helpers every generated client declares (ProtocolSupport error/numeric-parse support). */
  private static final List<String> CLIENT_FIXED =
      List.of(
          "ParseError",
          "ParsedError",
          "GenericError",
          "SanitizeErrorCode",
          "ParseInt64Text",
          "ParseDoubleText");

  /** Helpers every generated server declares (validation + error-response support). */
  private static final List<String> SERVER_FIXED =
      List.of(
          "ErrorToResponse",
          "AddValidationFailure",
          "ValidationErrorResponse",
          "ParseInt64Text",
          "ParseDoubleText");

  static void reject(
      CppContext context,
      ProtocolGenerator protocol,
      ServiceShape service,
      List<OperationShape> operations) {
    if (operations.isEmpty()) {
      return;
    }
    CppSettings settings = context.settings();
    List<OperationShape> streaming =
        EventStreamCodeGen.streamingOperations(context.model(), operations);
    Map<String, String> reserved = new HashMap<>();

    if (settings.generateClient()) {
      for (String name : CLIENT_FIXED) {
        reserved.put(name, "the generated client's " + name + " helper");
      }
      for (OperationShape operation : operations) {
        String opName = CppReservedWords.escape(operation.getId().getName());
        // Parse<Op>Error exists only for unary operations that declare errors;
        // error-less ones return GenericError, streaming ones report on the
        // stream (mirrors #69's emission scoping).
        if (!streaming.contains(operation) && !operation.getErrors(service).isEmpty()) {
          reserved.putIfAbsent(
              "Parse" + opName + "Error",
              "the generated client's Parse" + opName + "Error helper (" + operation.getId() + ")");
        }
        for (ShapeId errorId : operation.getErrors(service)) {
          String type =
              context.cppSymbols().toSymbol(context.model().expectShape(errorId)).getName();
          reserved.putIfAbsent(
              "Make" + type + "Error",
              "the generated client's Make" + type + "Error helper (" + errorId + ")");
        }
      }
    }

    if (settings.generateServer()) {
      for (String name : SERVER_FIXED) {
        reserved.putIfAbsent(name, "the generated server's " + name + " helper");
      }
      String errorHelper = protocol.serverErrorHelperName();
      reserved.putIfAbsent(errorHelper, "the generated server's " + errorHelper + " helper");
      for (OperationShape operation : operations) {
        String opName = CppReservedWords.escape(operation.getId().getName());
        for (String helper :
            protocol.serverOperationHelperNames(opName, streaming.contains(operation))) {
          reserved.putIfAbsent(
              helper, "the generated server's " + helper + " helper (" + operation.getId() + ")");
        }
      }
      for (String validator : new ValidationGenerator(context, operations).validatorNames()) {
        reserved.putIfAbsent(
            validator, "the generated server's " + validator + " validation helper");
      }
    }

    if (reserved.isEmpty()) {
      return;
    }
    for (Shape shape :
        new Walker(context.model()).walkShapes(context.model().expectShape(settings.service()))) {
      if (!(shape.isStructureShape()
          || shape.isUnionShape()
          || shape.isEnumShape()
          || shape.isIntEnumShape()
          || shape.isListShape()
          || shape.isMapShape())) {
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
}
