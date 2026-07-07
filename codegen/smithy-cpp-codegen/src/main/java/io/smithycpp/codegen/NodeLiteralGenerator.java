package io.smithycpp.codegen;

import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.node.ArrayNode;
import software.amazon.smithy.model.node.Node;
import software.amazon.smithy.model.node.NumberNode;
import software.amazon.smithy.model.node.ObjectNode;
import software.amazon.smithy.model.node.StringNode;
import software.amazon.smithy.model.shapes.ListShape;
import software.amazon.smithy.model.shapes.MapShape;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.shapes.UnionShape;
import software.amazon.smithy.model.traits.SparseTrait;
import software.amazon.smithy.utils.CaseUtils;

/**
 * Turns a Smithy {@link Node} (protocol test {@code params}) into a C++ expression that constructs
 * the equivalent value of a generated type. Used by {@link ProtocolTestGenerator}.
 */
final class NodeLiteralGenerator {

  private final CppContext context;

  NodeLiteralGenerator(CppContext context) {
    this.context = context;
  }

  private String typeName(Shape shape) {
    return context.cppSymbols().toSymbol(shape).getName();
  }

  private Shape target(MemberShape member) {
    return context.model().expectShape(member.getTarget());
  }

  /** C++ expression constructing {@code shape}'s generated type from {@code node}. */
  String expression(Shape shape, Node node) {
    return switch (shape.getType()) {
      case STRING -> CppLiterals.stringLiteral(node.expectStringNode().getValue());
      case BOOLEAN -> node.expectBooleanNode().getValue() ? "true" : "false";
      case BYTE, SHORT, INTEGER -> String.valueOf(node.expectNumberNode().getValue().intValue());
      case LONG -> node.expectNumberNode().getValue().longValue() + "LL";
      case FLOAT -> floatingExpression(node, true);
      case DOUBLE -> floatingExpression(node, false);
      case TIMESTAMP -> timestampExpression(node);
      case BLOB ->
          "smithy::Blob::FromString("
              + CppLiterals.stringLiteral(node.expectStringNode().getValue())
              + ")";
      case DOCUMENT -> documentExpression(node);
      case ENUM ->
          typeName(shape)
              + "::FromString("
              + CppLiterals.stringLiteral(node.expectStringNode().getValue())
              + ")";
      case INT_ENUM ->
          "static_cast<"
              + typeName(shape)
              + ">("
              + node.expectNumberNode().getValue().intValue()
              + ")";
      case STRUCTURE -> structureExpression(shape.asStructureShape().orElseThrow(), node);
      case UNION -> unionExpression(shape.asUnionShape().orElseThrow(), node);
      case LIST -> listExpression(shape.asListShape().orElseThrow(), node);
      case MAP -> mapExpression(shape.asMapShape().orElseThrow(), node);
      default ->
          throw new CodegenException(
              "cpp-codegen: protocol test literal for " + shape.getId() + " is not supported");
    };
  }

  private static String floatingExpression(Node node, boolean isFloat) {
    String type = isFloat ? "float" : "double";
    if (node.isStringNode()) {
      String text = node.expectStringNode().getValue();
      return switch (text) {
        case "NaN" -> "std::numeric_limits<" + type + ">::quiet_NaN()";
        case "Infinity" -> "std::numeric_limits<" + type + ">::infinity()";
        case "-Infinity" -> "-std::numeric_limits<" + type + ">::infinity()";
        default ->
            throw new CodegenException(
                "cpp-codegen: unexpected float text in test params: " + text);
      };
    }
    double value = node.expectNumberNode().getValue().doubleValue();
    return isFloat ? CppLiterals.floatLiteral(value) : CppLiterals.doubleLiteral(value);
  }

  private static String timestampExpression(Node node) {
    NumberNode number = node.expectNumberNode();
    long millis = Math.round(number.getValue().doubleValue() * 1000.0);
    return "smithy::Timestamp::FromEpochMilliseconds(" + millis + "LL)";
  }

  private String structureExpression(StructureShape shape, Node node) {
    ObjectNode object = node.expectObjectNode();
    if (shape.getId().toString().equals("smithy.api#Unit")) {
      return "smithy::Unit{}";
    }
    StringBuilder out = new StringBuilder("[] {\n");
    out.append("  ").append(typeName(shape)).append(" v{};\n");
    for (MemberShape member : shape.members()) {
      Node value = object.getMember(member.getMemberName()).orElse(null);
      if (value == null || value.isNullNode()) {
        continue;
      }
      out.append("  v.")
          .append(context.cppSymbols().toMemberName(member))
          .append(" = ")
          .append(expression(target(member), value))
          .append(";\n");
    }
    return out.append("  return v;\n}()").toString();
  }

  private String unionExpression(UnionShape shape, Node node) {
    ObjectNode object = node.expectObjectNode();
    for (MemberShape member : shape.members()) {
      Node value = object.getMember(member.getMemberName()).orElse(null);
      if (value == null) {
        continue;
      }
      String factory = CaseUtils.toPascalCase(context.cppSymbols().toMemberName(member));
      return typeName(shape) + "::From" + factory + "(" + expression(target(member), value) + ")";
    }
    throw new CodegenException(
        "cpp-codegen: union test params set no known member of " + shape.getId());
  }

  private String listExpression(ListShape shape, Node node) {
    ArrayNode array = node.expectArrayNode();
    boolean sparse = shape.hasTrait(SparseTrait.class);
    StringBuilder out = new StringBuilder(typeName(shape)).append('{');
    boolean first = true;
    for (Node element : array.getElements()) {
      if (!first) {
        out.append(", ");
      }
      first = false;
      if (element.isNullNode()) {
        if (!sparse) {
          throw new CodegenException("cpp-codegen: null element in dense list " + shape.getId());
        }
        out.append("std::nullopt");
      } else {
        out.append(expression(target(shape.getMember()), element));
      }
    }
    return out.append('}').toString();
  }

  private String mapExpression(MapShape shape, Node node) {
    ObjectNode object = node.expectObjectNode();
    boolean sparse = shape.hasTrait(SparseTrait.class);
    StringBuilder out = new StringBuilder(typeName(shape)).append('{');
    boolean first = true;
    for (var entry : object.getStringMap().entrySet()) {
      if (!first) {
        out.append(", ");
      }
      first = false;
      out.append('{').append(CppLiterals.stringLiteral(entry.getKey())).append(", ");
      if (entry.getValue().isNullNode()) {
        if (!sparse) {
          throw new CodegenException("cpp-codegen: null value in dense map " + shape.getId());
        }
        out.append("std::nullopt");
      } else {
        out.append(expression(target(shape.getValue()), entry.getValue()));
      }
      out.append('}');
    }
    return out.append('}').toString();
  }

  /**
   * The smallest valid C++ value of a shape: default-constructed, except where defaults do not
   * satisfy serde (required unions need a member, documents must be non-null). Used by the
   * generated service smoke tests for inputs and stub-handler outputs.
   */
  String minimalExpression(Shape shape) {
    return switch (shape.getType()) {
      case BOOLEAN -> "false";
      case BYTE, SHORT, INTEGER, INT_ENUM ->
          shape.getType() == software.amazon.smithy.model.shapes.ShapeType.INT_ENUM
              ? "static_cast<" + typeName(shape) + ">(0)"
              : "0";
      case LONG -> "0LL";
      case FLOAT -> "0.0F";
      case DOUBLE -> "0.0";
      case STRING -> "\"\"";
      case ENUM -> typeName(shape) + "::FromString(\"\")";
      case BLOB -> "smithy::Blob()";
      case TIMESTAMP -> "smithy::Timestamp::FromEpochMilliseconds(0)";
      case DOCUMENT -> "smithy::Document(smithy::DocumentMap{})";
      case LIST, MAP -> typeName(shape) + "{}";
      case STRUCTURE -> minimalStructureExpression(shape.asStructureShape().orElseThrow());
      case UNION -> minimalUnionExpression(shape.asUnionShape().orElseThrow());
      default -> throw new CodegenException("cpp-codegen: no minimal value for " + shape.getId());
    };
  }

  private String minimalStructureExpression(StructureShape shape) {
    if (shape.getId().toString().equals("smithy.api#Unit")) {
      return "smithy::Unit{}";
    }
    // Default construction is already minimal unless a required member's
    // default is invalid on the wire (unions, documents, nested structures
    // containing them).
    StringBuilder out = new StringBuilder("[] {\n");
    out.append("  ").append(typeName(shape)).append(" v{};\n");
    for (MemberShape member : shape.members()) {
      if (!member.isRequired()) {
        continue;
      }
      Shape memberTarget = target(member);
      if (needsExplicitMinimal(memberTarget, new java.util.HashSet<>())) {
        out.append("  v.")
            .append(context.cppSymbols().toMemberName(member))
            .append(" = ")
            .append(minimalExpression(memberTarget))
            .append(";\n");
      }
    }
    return out.append("  return v;\n}()").toString();
  }

  private String minimalUnionExpression(UnionShape shape) {
    MemberShape first = shape.members().iterator().next();
    String factory = CaseUtils.toPascalCase(context.cppSymbols().toMemberName(first));
    return typeName(shape) + "::From" + factory + "(" + minimalExpression(target(first)) + ")";
  }

  /** True when a default-constructed value of this shape does not deserialize. */
  private boolean needsExplicitMinimal(
      Shape shape, java.util.Set<software.amazon.smithy.model.shapes.ShapeId> visiting) {
    if (!visiting.add(shape.getId())) {
      return false;
    }
    return switch (shape.getType()) {
      case UNION, DOCUMENT -> true;
      case STRUCTURE ->
          shape.members().stream()
              .anyMatch(m -> m.isRequired() && needsExplicitMinimal(target(m), visiting));
      default -> false;
    };
  }

  private String documentExpression(Node node) {
    if (node.isNullNode()) {
      return "smithy::Document(nullptr)";
    }
    if (node.isBooleanNode()) {
      return "smithy::Document(" + (node.expectBooleanNode().getValue() ? "true" : "false") + ")";
    }
    if (node.isNumberNode()) {
      NumberNode number = node.expectNumberNode();
      if (number.isFloatingPointNumber()) {
        return "smithy::Document("
            + CppLiterals.doubleLiteral(number.getValue().doubleValue())
            + ")";
      }
      return "smithy::Document(std::int64_t{" + number.getValue().longValue() + "})";
    }
    if (node.isStringNode()) {
      StringNode string = node.expectStringNode();
      return "smithy::Document(std::string(" + CppLiterals.stringLiteral(string.getValue()) + "))";
    }
    if (node.isArrayNode()) {
      StringBuilder out = new StringBuilder("[] {\n  smithy::DocumentList list;\n");
      for (Node element : node.expectArrayNode().getElements()) {
        out.append("  list.emplace_back(").append(documentExpression(element)).append(");\n");
      }
      return out.append("  return smithy::Document(std::move(list));\n}()").toString();
    }
    StringBuilder out = new StringBuilder("[] {\n  smithy::DocumentMap map;\n");
    for (var entry : node.expectObjectNode().getStringMap().entrySet()) {
      out.append("  map.emplace(")
          .append(CppLiterals.stringLiteral(entry.getKey()))
          .append(", ")
          .append(documentExpression(entry.getValue()))
          .append(");\n");
    }
    return out.append("  return smithy::Document(std::move(map));\n}()").toString();
  }
}
