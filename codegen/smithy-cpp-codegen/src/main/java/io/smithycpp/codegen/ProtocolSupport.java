package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.IdempotencyTokenTrait;

/** Emission helpers shared by the concrete protocol generators. */
final class ProtocolSupport {

  private ProtocolSupport() {}

  /** Emits the protocol's error deserializer; only the wire decode differs per protocol. */
  static void writeErrorDeserializer(CppWriter w, String decodeStatement) {
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
    w.openBlock("smithy::Error DeserializeError(const smithy::http::HttpResponse& response) {");
    w.write("std::string code = \"UnknownError\";");
    w.write("std::string message = \"HTTP \" + std::to_string(response.status);");
    w.write(decodeStatement);
    w.openBlock("if (doc.ok() && doc->is_map()) {");
    w.write("const smithy::Document* type = doc->Find(\"__type\");");
    w.write("if (type == nullptr) type = doc->Find(\"code\");");
    w.write(
        "if (type != nullptr && type->is_string()) code = "
            + "SanitizeErrorCode(type->as_string());");
    w.write("const smithy::Document* text = doc->Find(\"message\");");
    w.write("if (text != nullptr && text->is_string()) message = text->as_string();");
    w.closeBlock("}");
    w.write("const bool retryable = response.status >= 500;");
    w.write(
        "if (code == \"UnknownError\") "
            + "return smithy::Error(smithy::ErrorKind::kUnknown, code, message, retryable);");
    w.write("return smithy::Error::Modeled(std::move(code), std::move(message), retryable);");
    w.closeBlock("}");
    w.write("");
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
      case FLOAT, DOUBLE -> "std::to_string(" + valueExpr + ")";
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
        "\"smithy/core/uuid.h\"",
        "\"smithy/http/socket_transport.h\"",
        "\"smithy/http/uri.h\"",
        "<string>",
        "<string_view>",
        "<utility>");
  }
}
