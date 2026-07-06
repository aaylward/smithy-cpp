package io.smithycpp.codegen;

import java.util.Optional;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.traits.TimestampFormatTrait;

/**
 * Member-level serde emission shared by the serde file generator and the protocol client
 * generators: C++ expressions that turn a typed value into a Document node, and statements that
 * read a Document node back into a typed lvalue.
 */
final class SerdeCodeGen {

  private final CppContext context;

  SerdeCodeGen(CppContext context) {
    this.context = context;
  }

  private Shape target(MemberShape member) {
    return context.model().expectShape(member.getTarget());
  }

  static String serdeFunctionSuffix(CppContext context, Shape shape) {
    return context.cppSymbols().declaredName(shape);
  }

  /** C++ TimestampFormat constant for a member, honoring @timestampFormat. */
  String timestampFormat(MemberShape member) {
    return timestampFormat(member, "smithy::TimestampFormat::kEpochSeconds");
  }

  String timestampFormat(MemberShape member, String defaultConstant) {
    Optional<TimestampFormatTrait> trait =
        member
            .getTrait(TimestampFormatTrait.class)
            .or(() -> target(member).getTrait(TimestampFormatTrait.class));
    if (trait.isEmpty()) {
      return defaultConstant;
    }
    return switch (trait.get().getValue()) {
      case "date-time" -> "smithy::TimestampFormat::kDateTime";
      case "http-date" -> "smithy::TimestampFormat::kHttpDate";
      default -> "smithy::TimestampFormat::kEpochSeconds";
    };
  }

  /** Expression producing a smithy::Document from {@code valueExpr} of the member's type. */
  String serializeExpression(MemberShape member, String valueExpr) {
    Shape shape = target(member);
    if (shape.getId().toString().equals("smithy.api#Unit")) {
      return "smithy::Document(smithy::DocumentMap{})";
    }
    return switch (shape.getType()) {
      case BOOLEAN -> "smithy::Document(" + valueExpr + ")";
      case BYTE, SHORT, INTEGER, LONG ->
          "smithy::Document(static_cast<std::int64_t>(" + valueExpr + "))";
      case INT_ENUM -> "smithy::Document(static_cast<std::int64_t>(" + valueExpr + "))";
      case FLOAT, DOUBLE -> "smithy::Document(static_cast<double>(" + valueExpr + "))";
      case STRING -> "smithy::Document(" + valueExpr + ")";
      case ENUM -> "smithy::Document(std::string(" + valueExpr + ".ToString()))";
      case BLOB -> "smithy::Document(" + valueExpr + ")";
      case TIMESTAMP ->
          "smithy::Document::FromTimestamp(" + valueExpr + ", " + timestampFormat(member) + ")";
      case DOCUMENT -> valueExpr;
      case STRUCTURE, UNION, LIST, MAP ->
          "Serialize" + serdeFunctionSuffix(context, shape) + "(" + valueExpr + ")";
      default ->
          throw new CodegenException(
              "cpp-codegen: cannot serialize member targeting " + shape.getId());
    };
  }

  /**
   * Emits statements reading {@code *docExpr} (a non-null, non-null-node Document pointer) into
   * {@code outExpr}, returning a serialization Error on mismatch. {@code path} names the member in
   * error messages.
   */
  void writeDeserializeInto(
      CppWriter w, MemberShape member, String docExpr, String outExpr, String path) {
    Shape shape = target(member);
    String wrong =
        "return smithy::Error::Serialization(\"" + path + ": unexpected type on the wire\");";
    if (shape.getId().toString().equals("smithy.api#Unit")) {
      w.write("$L = smithy::Unit{};", outExpr);
      return;
    }
    switch (shape.getType()) {
      case BOOLEAN -> {
        w.write("if (!$L->is_bool()) $L", docExpr, wrong);
        w.write("$L = $L->as_bool();", outExpr, docExpr);
      }
      case BYTE, SHORT, INTEGER, LONG -> {
        String type = context.cppSymbols().toSymbol(shape).getName();
        w.write("if (!$L->is_int()) $L", docExpr, wrong);
        w.write("$L = static_cast<$L>($L->as_int());", outExpr, type, docExpr);
      }
      case INT_ENUM -> {
        String type = context.cppSymbols().toSymbol(shape).getName();
        w.write("if (!$L->is_int()) $L", docExpr, wrong);
        w.write("$L = static_cast<$L>($L->as_int());", outExpr, type, docExpr);
      }
      case FLOAT, DOUBLE -> {
        String type = context.cppSymbols().toSymbol(shape).getName();
        w.openBlock("{");
        w.write("auto parsed = smithy::DoubleFromDocument(*$L);", docExpr);
        w.write(
            "if (!parsed) return smithy::Error::Serialization($S);", path + ": expected a number");
        w.write("$L = static_cast<$L>(*parsed);", outExpr, type);
        w.closeBlock("}");
      }
      case STRING -> {
        w.write("if (!$L->is_string()) $L", docExpr, wrong);
        w.write("$L = $L->as_string();", outExpr, docExpr);
      }
      case ENUM -> {
        String type = context.cppSymbols().toSymbol(shape).getName();
        w.write("if (!$L->is_string()) $L", docExpr, wrong);
        w.write("$L = $L::FromString($L->as_string());", outExpr, type, docExpr);
      }
      case BLOB -> {
        w.openBlock("{");
        w.write("auto parsed = smithy::BlobFromDocument(*$L);", docExpr);
        w.write("if (!parsed) return std::move(parsed).error();");
        w.write("$L = std::move(*parsed);", outExpr);
        w.closeBlock("}");
      }
      case TIMESTAMP -> {
        w.openBlock("{");
        w.write(
            "auto parsed = smithy::TimestampFromDocument(*$L, $L);",
            docExpr,
            timestampFormat(member));
        w.write("if (!parsed) return std::move(parsed).error();");
        w.write("$L = *parsed;", outExpr);
        w.closeBlock("}");
      }
      case DOCUMENT -> w.write("$L = *$L;", outExpr, docExpr);
      case STRUCTURE, UNION, LIST, MAP -> {
        w.openBlock("{");
        w.write("auto parsed = Deserialize$L(*$L);", serdeFunctionSuffix(context, shape), docExpr);
        w.write("if (!parsed) return std::move(parsed).error();");
        w.write("$L = std::move(*parsed);", outExpr);
        w.closeBlock("}");
      }
      default ->
          throw new CodegenException(
              "cpp-codegen: cannot deserialize member targeting " + shape.getId());
    }
  }
}
