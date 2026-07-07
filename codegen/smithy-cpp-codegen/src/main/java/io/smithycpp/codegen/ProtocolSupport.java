package io.smithycpp.codegen;

import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.IdempotencyTokenTrait;
import software.amazon.smithy.model.traits.RetryableTrait;

/** Emission helpers shared by the concrete protocol generators. */
final class ProtocolSupport {

  private ProtocolSupport() {}

  /**
   * Emits the protocol's shared error-parsing helpers (SanitizeErrorCode, ParsedError, ParseError,
   * GenericError); only the wire decode and the code-carrying header differ per protocol.
   */
  static void writeErrorSupport(CppWriter w, String decodeStatement, boolean errorTypeHeader) {
    w.write("// The error shape name arrives namespaced (\"ns#Shape\") and possibly");
    w.write("// URI-qualified; modeled error codes keep only the shape name.");
    w.openBlock("std::string SanitizeErrorCode(std::string_view raw) {");
    w.write(
        "if (const auto colon = raw.find(':'); colon != std::string_view::npos) "
            + "raw = raw.substr(0, colon);");
    w.write(
        "if (const auto hash = raw.find('#'); hash != std::string_view::npos) "
            + "raw = raw.substr(hash + 1);");
    w.write("return std::string(raw);");
    w.closeBlock("}");
    w.write("");
    w.openBlock("struct ParsedError {");
    w.write("int status = 0;");
    w.write("std::string code = \"UnknownError\";");
    w.write("std::string message;");
    w.write("smithy::Document doc;");
    w.closeBlock("};");
    w.write("");
    w.openBlock("ParsedError ParseError(const smithy::http::HttpResponse& response) {");
    w.write("ParsedError parsed;");
    w.write("parsed.status = response.status;");
    w.write("parsed.message = \"HTTP \" + std::to_string(response.status);");
    w.write(decodeStatement);
    w.write("if (doc.ok()) parsed.doc = *std::move(doc);");
    if (errorTypeHeader) {
      w.write("const auto type_header = response.headers.Get(\"x-amzn-errortype\");");
      w.write("if (type_header.has_value()) parsed.code = SanitizeErrorCode(*type_header);");
    }
    w.openBlock("if (parsed.doc.is_map()) {");
    w.write("const smithy::Document* type = parsed.doc.Find(\"__type\");");
    w.write("if (type == nullptr) type = parsed.doc.Find(\"code\");");
    w.write(
        "if (parsed.code == \"UnknownError\" && type != nullptr && type->is_string()) "
            + "parsed.code = SanitizeErrorCode(type->as_string());");
    w.write("const smithy::Document* text = parsed.doc.Find(\"message\");");
    w.write("if (text != nullptr && text->is_string()) parsed.message = text->as_string();");
    w.closeBlock("}");
    w.write("return parsed;");
    w.closeBlock("}");
    w.write("");
    w.openBlock("smithy::Error GenericError(ParsedError parsed) {");
    w.write("const bool retryable = parsed.status >= 500;");
    w.write(
        "if (parsed.code == \"UnknownError\") return smithy::Error(smithy::ErrorKind::kUnknown, "
            + "std::move(parsed.code), std::move(parsed.message), retryable);");
    w.write(
        "return smithy::Error::Modeled(std::move(parsed.code), std::move(parsed.message), "
            + "retryable);");
    w.closeBlock("}");
    w.write("");
  }

  /** Text-to-number helpers used for header/label/query bindings. */
  static void writeNumericParseHelpers(CppWriter w) {
    w.write("// Text value parsing for label/query/header bindings ([[maybe_unused]]:");
    w.write("// emitted for every service; not every service binds numeric values).");
    w.openBlock("[[maybe_unused]] std::int64_t ParseHeaderInt64(const std::string& text) {");
    w.write("return std::strtoll(text.c_str(), nullptr, 10);");
    w.closeBlock("}");
    w.write("");
    w.openBlock("[[maybe_unused]] double ParseHeaderDouble(const std::string& text) {");
    w.write("if (text == \"NaN\") return std::numeric_limits<double>::quiet_NaN();");
    w.write("if (text == \"Infinity\") return std::numeric_limits<double>::infinity();");
    w.write("if (text == \"-Infinity\") return -std::numeric_limits<double>::infinity();");
    w.write("return std::strtod(text.c_str(), nullptr);");
    w.closeBlock("}");
    w.write("");
  }

  /** HTTP status for a modeled error shape: @httpError, else @error class default. */
  static int errorStatus(StructureShape shape) {
    var httpError = shape.getTrait(software.amazon.smithy.model.traits.HttpErrorTrait.class);
    if (httpError.isPresent()) {
      return httpError.get().getCode();
    }
    var error = shape.expectTrait(software.amazon.smithy.model.traits.ErrorTrait.class);
    return error.isClientError() ? 400 : 500;
  }

  /**
   * Emits the server's ErrorToResponse over {@code errorBodyFn(status, code, message, body)}:
   * modeled errors get their @httpError status and serialized detail; validation/serialization
   * failures map to 400; anything else is a non-leaking 500.
   */
  static void writeServerErrorToResponse(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      List<OperationShape> operations,
      String errorBodyFn) {
    Map<String, StructureShape> errorShapes = new TreeMap<>();
    for (OperationShape operation : operations) {
      for (ShapeId errorId : operation.getErrors(service)) {
        StructureShape shape =
            context.model().expectShape(errorId).asStructureShape().orElseThrow();
        errorShapes.put(context.cppSymbols().toSymbol(shape).getName(), shape);
      }
    }
    w.openBlock("smithy::http::HttpResponse ErrorToResponse(const smithy::Error& error) {");
    w.openBlock("if (error.kind() == smithy::ErrorKind::kModeled) {");
    for (StructureShape shape : errorShapes.values()) {
      String type = context.cppSymbols().toSymbol(shape).getName();
      w.openBlock("if (error.code() == $S) {", shape.getId().getName());
      w.write("smithy::DocumentMap body;");
      w.openBlock("if (const auto* detail = error.detail<$L>()) {", type);
      w.write(
          "body = Serialize$L(*detail).as_map();",
          SerdeCodeGen.serdeFunctionSuffix(context, shape));
      w.closeBlock("}");
      w.write(
          "return $L($L, error.code(), error.message(), std::move(body));",
          errorBodyFn,
          errorStatus(shape));
      w.closeBlock("}");
    }
    w.write("return $L(400, error.code(), error.message(), {});", errorBodyFn);
    w.closeBlock("}");
    w.write(
        "if (error.kind() == smithy::ErrorKind::kValidation || error.kind() == "
            + "smithy::ErrorKind::kSerialization) return $L(400, \"ValidationException\", "
            + "error.message(), {});",
        errorBodyFn);
    w.write("// Never leak internal detail on unexpected failures.");
    w.write("return $L(500, \"InternalFailure\", \"internal failure\", {});", errorBodyFn);
    w.closeBlock("}");
    w.write("");
  }

  static List<String> sharedServerIncludes(CppContext context) {
    return List.of(
        "\"" + context.settings().includePrefix() + "/server.h\"",
        "\"" + context.settings().includePrefix() + "/serde.h\"",
        "\"smithy/server/router.h\"",
        "<memory>",
        "<string>",
        "<string_view>",
        "<utility>");
  }

  /**
   * Emits Make&lt;Error&gt;Error for every error shape any operation declares (typed detail via the
   * shape's serde, @retryable honored) plus a Deserialize&lt;Op&gt;Error dispatcher per operation
   * that has errors. Must run inside the client source's anonymous namespace, after {@link
   * #writeErrorSupport}.
   */
  static void writeOperationErrorDeserializers(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> operations) {
    Map<String, StructureShape> errorShapes = new TreeMap<>();
    for (OperationShape operation : operations) {
      for (ShapeId errorId : operation.getErrors(service)) {
        StructureShape shape =
            context.model().expectShape(errorId).asStructureShape().orElseThrow();
        errorShapes.put(context.cppSymbols().toSymbol(shape).getName(), shape);
      }
    }
    for (StructureShape shape : errorShapes.values()) {
      String type = context.cppSymbols().toSymbol(shape).getName();
      boolean retryable = shape.hasTrait(RetryableTrait.class);
      w.openBlock(
          "smithy::Error Make$LError(const smithy::http::HttpResponse& response, "
              + "ParsedError parsed) {",
          type);
      w.write("(void)response;");
      if (retryable) {
        w.write("const bool retryable = true;  // @retryable");
      } else {
        w.write("const bool retryable = parsed.status >= 500;");
      }
      // code() carries the wire-level shape name, which can differ from the
      // C++ type name when a foreign-namespace shape was disambiguated.
      w.write(
          "smithy::Error error = smithy::Error::Modeled($S, std::move(parsed.message), "
              + "retryable);",
          shape.getId().getName());
      // Errors with header-only payloads (or none at all) may have no body:
      // deserialize from an empty map so the typed detail still attaches.
      w.write("if (!parsed.doc.is_map()) parsed.doc = smithy::Document(smithy::DocumentMap{});");
      w.write(
          "auto detail = Deserialize$L(parsed.doc);",
          SerdeCodeGen.serdeFunctionSuffix(context, shape));
      w.openBlock("if (detail.ok()) {");
      protocol.writeErrorDetailPatches(w, context, shape);
      w.write("error.set_detail(*std::move(detail));");
      w.closeBlock("}");
      w.write("return error;");
      w.closeBlock("}");
      w.write("");
    }
    for (OperationShape operation : operations) {
      List<ShapeId> errors = operation.getErrors(service);
      if (errors.isEmpty()) {
        continue;
      }
      Map<String, StructureShape> sorted = new TreeMap<>();
      for (ShapeId errorId : errors) {
        StructureShape shape =
            context.model().expectShape(errorId).asStructureShape().orElseThrow();
        sorted.put(context.cppSymbols().toSymbol(shape).getName(), shape);
      }
      w.openBlock(
          "smithy::Error Deserialize$LError(const smithy::http::HttpResponse& response) {",
          CppReservedWords.escape(operation.getId().getName()));
      w.write("ParsedError parsed = ParseError(response);");
      for (Map.Entry<String, StructureShape> entry : sorted.entrySet()) {
        w.write(
            "if (parsed.code == $S) return Make$LError(response, std::move(parsed));",
            entry.getValue().getId().getName(),
            entry.getKey());
      }
      w.write("return GenericError(std::move(parsed));");
      w.closeBlock("}");
      w.write("");
    }
  }

  /** The expression a protocol's operation body returns for a non-success response. */
  static String errorExpression(CppContext context, ServiceShape service, OperationShape op) {
    if (op.getErrors(service).isEmpty()) {
      return "GenericError(ParseError(*response))";
    }
    return "Deserialize" + CppReservedWords.escape(op.getId().getName()) + "Error(*response)";
  }

  /**
   * If the input has @idempotencyToken members, emits a prepared copy with unset tokens filled and
   * returns the expression to use for the input from then on.
   */
  static String prepareIdempotencyTokens(
      CppWriter w, CppContext context, StructureShape input, String inputType) {
    boolean any = false;
    for (MemberShape member : input.members()) {
      if (!member.hasTrait(IdempotencyTokenTrait.class)) {
        continue;
      }
      if (!any) {
        w.write("$L prepared = input;", inputType);
        any = true;
      }
      String field = "prepared." + context.cppSymbols().toMemberName(member);
      if (member.isRequired()) {
        w.write("if ($L.empty()) $L = smithy::GenerateUuidV4();", field, field);
      } else {
        w.write("if (!$L.has_value()) $L = smithy::GenerateUuidV4();", field, field);
      }
    }
    return any ? "prepared" : "input";
  }

  /** String conversion for label/query/header values of simple types. */
  static String toStringExpression(
      CppContext context, MemberShape member, String valueExpr, String timestampFormat) {
    Shape target = context.model().expectShape(member.getTarget());
    return switch (target.getType()) {
      case STRING -> valueExpr;
      case ENUM -> "std::string(" + valueExpr + ".ToString())";
      case BYTE, SHORT, INTEGER, LONG, INT_ENUM ->
          "std::to_string(static_cast<std::int64_t>(" + valueExpr + "))";
      case FLOAT -> "smithy::FormatFloat(" + valueExpr + ")";
      case DOUBLE -> "smithy::FormatDouble(" + valueExpr + ")";
      case BOOLEAN -> "(" + valueExpr + " ? \"true\" : \"false\")";
      case TIMESTAMP -> valueExpr + ".Format(" + timestampFormat + ")";
      default ->
          throw new CodegenException("cpp-codegen: unsupported binding target " + target.getId());
    };
  }

  static StructureShape inputShape(CppContext context, OperationShape operation) {
    return context.model().expectShape(operation.getInputShape()).asStructureShape().orElseThrow();
  }

  static StructureShape outputShape(CppContext context, OperationShape operation) {
    return context.model().expectShape(operation.getOutputShape()).asStructureShape().orElseThrow();
  }

  static List<String> sharedClientIncludes(CppContext context) {
    return List.of(
        "\"" + context.settings().includePrefix() + "/client.h\"",
        "\"" + context.settings().includePrefix() + "/serde.h\"",
        "\"smithy/core/blob.h\"",
        "\"smithy/core/document_serde.h\"",
        "\"smithy/core/uuid.h\"",
        "\"smithy/http/socket_transport.h\"",
        "\"smithy/http/uri.h\"",
        "<string>",
        "<string_view>",
        "<utility>");
  }
}
