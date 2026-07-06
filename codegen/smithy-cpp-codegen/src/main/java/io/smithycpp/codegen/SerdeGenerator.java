package io.smithycpp.codegen;

import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import software.amazon.smithy.codegen.core.Symbol;
import software.amazon.smithy.codegen.core.TopologicalIndex;
import software.amazon.smithy.model.neighbor.Walker;
import software.amazon.smithy.model.shapes.ListShape;
import software.amazon.smithy.model.shapes.MapShape;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.shapes.UnionShape;
import software.amazon.smithy.model.traits.SparseTrait;

/**
 * Emits serde.h / src/serde.cc: Document-pivot serializers and deserializers for every aggregate
 * shape in the service closure (both directions — clients, servers, and tests all reuse them).
 */
final class SerdeGenerator {

  private final CppContext context;
  private final SerdeCodeGen serde;

  SerdeGenerator(CppContext context) {
    this.context = context;
    this.serde = new SerdeCodeGen(context);
  }

  /** Aggregate shapes in the closure, topologically ordered, excluding smithy.api#Unit. */
  List<Shape> serdeShapes() {
    Set<Shape> closure =
        new Walker(context.model())
            .walkShapes(context.model().expectShape(context.settings().service()));
    List<Shape> ordered = new ArrayList<>();
    for (Shape shape : TopologicalIndex.of(context.model()).getOrderedShapes()) {
      if (!closure.contains(shape) || shape.getId().toString().equals("smithy.api#Unit")) {
        continue;
      }
      if (shape.isStructureShape()
          || shape.isUnionShape()
          || shape.isListShape()
          || shape.isMapShape()) {
        ordered.add(shape);
      }
    }
    // Recursive-shape rejection guarantees the topological index covered everything.
    return ordered;
  }

  void run() {
    List<Shape> shapes = serdeShapes();
    if (shapes.isEmpty()) {
      return;
    }
    CppSettings settings = context.settings();
    context
        .writerDelegator()
        .useFileWriter(settings.serdeHeaderFile(), w -> writeHeader(w, shapes));
    context.writerDelegator().useFileWriter("src/serde.cc", w -> writeSource(w, shapes));
  }

  private String valueType(Shape shape) {
    return context.cppSymbols().toSymbol(shape).getName();
  }

  private void writeHeader(CppWriter w, List<Shape> shapes) {
    w.addInclude("\"smithy/core/document.h\"");
    w.addInclude("\"smithy/core/outcome.h\"");
    w.addInclude("\"" + context.settings().includePrefix() + "/types.h\"");
    w.write("// Document-pivot serde for every aggregate shape in the model closure.");
    w.write("// Serializers never fail; deserializers return smithy::Error on wire");
    w.write("// mismatches and enforce @required members.");
    w.write("");
    for (Shape shape : shapes) {
      String suffix = SerdeCodeGen.serdeFunctionSuffix(shape);
      String type = valueType(shape);
      w.addIncludesFor(context.cppSymbols().toSymbol(shape));
      w.write("smithy::Document Serialize$L(const $L& value);", suffix, type);
      w.write("smithy::Outcome<$L> Deserialize$L(const smithy::Document& doc);", type, suffix);
      w.write("");
    }
  }

  private void writeSource(CppWriter w, List<Shape> shapes) {
    w.addInclude("\"" + context.settings().includePrefix() + "/serde.h\"");
    w.addInclude("\"smithy/core/document_serde.h\"");
    w.addInclude("<string>");
    w.addInclude("<utility>");
    for (Shape shape : shapes) {
      if (shape.isStructureShape()) {
        writeStructure(w, shape.asStructureShape().orElseThrow());
      } else if (shape.isUnionShape()) {
        writeUnion(w, shape.asUnionShape().orElseThrow());
      } else if (shape.isListShape()) {
        writeList(w, shape.asListShape().orElseThrow());
      } else {
        writeMap(w, shape.asMapShape().orElseThrow());
      }
    }
  }

  private void writeStructure(CppWriter w, StructureShape shape) {
    String suffix = SerdeCodeGen.serdeFunctionSuffix(shape);
    String type = valueType(shape);

    w.openBlock("smithy::Document Serialize$L(const $L& value) {", suffix, type);
    w.write("smithy::DocumentMap map;");
    for (MemberShape member : shape.members()) {
      String field = "value." + context.cppSymbols().toMemberName(member);
      if (member.isRequired()) {
        w.write(
            "map.emplace($S, $L);",
            member.getMemberName(),
            serde.serializeExpression(member, field));
      } else {
        w.openBlock("if ($L.has_value()) {", field);
        w.write(
            "map.emplace($S, $L);",
            member.getMemberName(),
            serde.serializeExpression(member, "*" + field));
        w.closeBlock("}");
      }
    }
    w.write("return smithy::Document(std::move(map));");
    w.closeBlock("}");
    w.write("");

    w.openBlock("smithy::Outcome<$L> Deserialize$L(const smithy::Document& doc) {", type, suffix);
    w.write(
        "if (!doc.is_map()) return smithy::Error::Serialization($S);",
        type + ": expected a map on the wire");
    w.write("$L out;", type);
    for (MemberShape member : shape.members()) {
      String name = member.getMemberName();
      String field = "out." + context.cppSymbols().toMemberName(member);
      String path = type + "." + name;
      w.openBlock("{");
      w.write("const smithy::Document* member = doc.Find($S);", name);
      if (member.isRequired()) {
        w.openBlock("if (member == nullptr || member->is_null()) {");
        w.write(
            "return smithy::Error::Serialization($S);",
            type + ": missing required member: " + name);
        w.closeBlock("}");
        serde.writeDeserializeInto(w, member, "member", field, path);
      } else {
        w.openBlock("if (member != nullptr && !member->is_null()) {");
        Symbol targetType =
            context.cppSymbols().toSymbol(context.model().expectShape(member.getTarget()));
        w.write("$L parsed_member{};", targetType.getName());
        serde.writeDeserializeInto(w, member, "member", "parsed_member", path);
        w.write("$L = std::move(parsed_member);", field);
        w.closeBlock("}");
      }
      w.closeBlock("}");
    }
    w.write("return out;");
    w.closeBlock("}");
    w.write("");
  }

  private void writeUnion(CppWriter w, UnionShape shape) {
    String suffix = SerdeCodeGen.serdeFunctionSuffix(shape);
    String type = valueType(shape);

    w.openBlock("smithy::Document Serialize$L(const $L& value) {", suffix, type);
    w.write("smithy::DocumentMap map;");
    for (MemberShape member : shape.members()) {
      String name = context.cppSymbols().toMemberName(member);
      w.openBlock("if (value.is_$L()) {", name);
      w.write(
          "map.emplace($S, $L);",
          member.getMemberName(),
          serde.serializeExpression(member, "value.as_" + name + "()"));
      w.closeBlock("}");
    }
    w.write("return smithy::Document(std::move(map));");
    w.closeBlock("}");
    w.write("");

    w.openBlock("smithy::Outcome<$L> Deserialize$L(const smithy::Document& doc) {", type, suffix);
    w.write(
        "if (!doc.is_map()) return smithy::Error::Serialization($S);",
        type + ": expected a map on the wire");
    for (MemberShape member : shape.members()) {
      String wireName = member.getMemberName();
      Symbol targetType =
          context.cppSymbols().toSymbol(context.model().expectShape(member.getTarget()));
      w.openBlock(
          "if (const smithy::Document* member = doc.Find($S);"
              + " member != nullptr && !member->is_null()) {",
          wireName);
      w.write("$L parsed_member{};", targetType.getName());
      serde.writeDeserializeInto(w, member, "member", "parsed_member", type + "." + wireName);
      w.write(
          "return $L::From$L(std::move(parsed_member));",
          type,
          pascal(context.cppSymbols().toMemberName(member)));
      w.closeBlock("}");
    }
    w.write("return smithy::Error::Serialization($S);", type + ": unknown or missing union member");
    w.closeBlock("}");
    w.write("");
  }

  private void writeList(CppWriter w, ListShape shape) {
    String suffix = SerdeCodeGen.serdeFunctionSuffix(shape);
    String type = valueType(shape);
    MemberShape member = shape.getMember();
    boolean sparse = shape.hasTrait(SparseTrait.class);
    Symbol element = context.cppSymbols().toSymbol(context.model().expectShape(member.getTarget()));

    w.openBlock("smithy::Document Serialize$L(const $L& value) {", suffix, type);
    w.write("smithy::DocumentList list;");
    w.write("list.reserve(value.size());");
    w.openBlock("for (const auto& item : value) {");
    if (sparse) {
      w.openBlock("if (!item.has_value()) {");
      w.write("list.emplace_back(nullptr);");
      w.write("continue;");
      w.closeBlock("}");
      w.write("list.push_back($L);", serde.serializeExpression(member, "*item"));
    } else {
      w.write("list.push_back($L);", serde.serializeExpression(member, "item"));
    }
    w.closeBlock("}");
    w.write("return smithy::Document(std::move(list));");
    w.closeBlock("}");
    w.write("");

    w.openBlock("smithy::Outcome<$L> Deserialize$L(const smithy::Document& doc) {", type, suffix);
    w.write(
        "if (!doc.is_list()) return smithy::Error::Serialization($S);",
        type + ": expected a list on the wire");
    w.write("$L out;", type);
    w.write("out.reserve(doc.as_list().size());");
    w.openBlock("for (const smithy::Document& item_doc : doc.as_list()) {");
    w.write("const smithy::Document* item = &item_doc;");
    if (sparse) {
      w.openBlock("if (item->is_null()) {");
      w.write("out.emplace_back(std::nullopt);");
      w.write("continue;");
      w.closeBlock("}");
    } else {
      w.write(
          "if (item->is_null()) return smithy::Error::Serialization($S);",
          type + ": null element in a dense list");
    }
    w.write("$L parsed_item{};", element.getName());
    serde.writeDeserializeInto(w, member, "item", "parsed_item", type + "[]");
    w.write("out.push_back(std::move(parsed_item));");
    w.closeBlock("}");
    w.write("return out;");
    w.closeBlock("}");
    w.write("");
  }

  private void writeMap(CppWriter w, MapShape shape) {
    String suffix = SerdeCodeGen.serdeFunctionSuffix(shape);
    String type = valueType(shape);
    MemberShape member = shape.getValue();
    boolean sparse = shape.hasTrait(SparseTrait.class);
    Symbol element = context.cppSymbols().toSymbol(context.model().expectShape(member.getTarget()));

    w.openBlock("smithy::Document Serialize$L(const $L& value) {", suffix, type);
    w.write("smithy::DocumentMap map;");
    w.openBlock("for (const auto& [key, item] : value) {");
    if (sparse) {
      w.openBlock("if (!item.has_value()) {");
      w.write("map.emplace(key, smithy::Document(nullptr));");
      w.write("continue;");
      w.closeBlock("}");
      w.write("map.emplace(key, $L);", serde.serializeExpression(member, "*item"));
    } else {
      w.write("map.emplace(key, $L);", serde.serializeExpression(member, "item"));
    }
    w.closeBlock("}");
    w.write("return smithy::Document(std::move(map));");
    w.closeBlock("}");
    w.write("");

    w.openBlock("smithy::Outcome<$L> Deserialize$L(const smithy::Document& doc) {", type, suffix);
    w.write(
        "if (!doc.is_map()) return smithy::Error::Serialization($S);",
        type + ": expected a map on the wire");
    w.write("$L out;", type);
    w.openBlock("for (const auto& [key, item_doc] : doc.as_map()) {");
    w.write("const smithy::Document* item = &item_doc;");
    if (sparse) {
      w.openBlock("if (item->is_null()) {");
      w.write("out.emplace(key, std::nullopt);");
      w.write("continue;");
      w.closeBlock("}");
    } else {
      w.write(
          "if (item->is_null()) return smithy::Error::Serialization($S);",
          type + ": null value in a dense map");
    }
    w.write("$L parsed_item{};", element.getName());
    serde.writeDeserializeInto(w, member, "item", "parsed_item", type + "{}");
    w.write("out.emplace(key, std::move(parsed_item));");
    w.closeBlock("}");
    w.write("return out;");
    w.closeBlock("}");
    w.write("");
  }

  static String pascal(String name) {
    return software.amazon.smithy.utils.CaseUtils.toPascalCase(name);
  }
}
