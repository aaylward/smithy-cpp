package io.smithycpp.codegen;

import java.util.List;
import java.util.Map;
import software.amazon.smithy.codegen.core.Symbol;
import software.amazon.smithy.model.shapes.EnumShape;
import software.amazon.smithy.model.shapes.IntEnumShape;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.shapes.UnionShape;
import software.amazon.smithy.model.traits.DocumentationTrait;
import software.amazon.smithy.utils.CaseUtils;

/** Emits C++ declarations for Smithy data shapes (the contract in docs/generated-types.md). */
final class TypeGenerators {

  private final CppContext context;
  private final CppWriter writer;

  TypeGenerators(CppContext context, CppWriter writer) {
    this.context = context;
    this.writer = writer;
  }

  private CppSymbolProvider symbols() {
    return context.cppSymbols();
  }

  private void writeDocs(Shape shape) {
    shape
        .getTrait(DocumentationTrait.class)
        .ifPresent(
            docs -> {
              for (String line : docs.getValue().split("\n", -1)) {
                writer.write("/// $L", line);
              }
            });
  }

  private String typeName(Shape shape) {
    return context.symbolProvider().toSymbol(shape).getName();
  }

  /** Enum constant for a member name: DRIP -> kDrip, dashEs-and_under -> kDashEsAndUnder. */
  static String enumConstant(String memberName) {
    return "k" + CaseUtils.toPascalCase(memberName.toLowerCase().replace('-', '_'));
  }

  /**
   * Forward declarations for recursive member targets: on a cycle, the target's definition may come
   * later in types.h. Boxed members and std::vector elements only need the name declared; duplicate
   * declarations are harmless.
   */
  private void writeForwardDeclarations(StructureShape shape) {
    RecursionIndex recursion = symbols().recursion();
    if (!recursion.inCycle(shape.getId())) {
      return;
    }
    java.util.Set<String> declared = new java.util.TreeSet<>();
    for (MemberShape member : shape.members()) {
      collectCyclicStructNames(shape, member, declared);
    }
    for (String name : declared) {
      writer.write("struct $L;", name);
    }
    if (!declared.isEmpty()) {
      writer.write("");
    }
  }

  /** Struct names the member's type text references from the containing shape's cycle. */
  private void collectCyclicStructNames(
      StructureShape container, MemberShape member, java.util.Set<String> out) {
    RecursionIndex recursion = symbols().recursion();
    Shape target = context.model().expectShape(member.getTarget());
    if (target.isStructureShape()) {
      if (recursion.inCycle(target.getId()) && recursion.inCycle(container.getId())) {
        out.add(typeName(target));
      }
      return;
    }
    // Lists/maps inline their element type (std::vector<T> / std::map<K, T>),
    // so a cyclic element still appears in this struct's member text.
    if (target.isListShape()) {
      collectCyclicStructNames(container, target.asListShape().orElseThrow().getMember(), out);
    } else if (target.isMapShape()) {
      collectCyclicStructNames(container, target.asMapShape().orElseThrow().getValue(), out);
    }
  }

  void generateStructure(StructureShape shape) {
    String name = typeName(shape);
    writeForwardDeclarations(shape);
    writeDocs(shape);
    writer.openBlock("struct $L {", name);
    for (MemberShape member : shape.members()) {
      Symbol type = symbols().toMemberSymbol(member);
      writer.addIncludesFor(type);
      writer.write("$L $L{};", type.getName(), symbols().toMemberName(member));
    }
    if (!shape.members().isEmpty()) {
      writer.write("");
    }
    writer.write("friend bool operator==(const $1L&, const $1L&) = default;", name);
    writer.closeBlock("};");
    writer.write("");
  }

  void generateEnum(EnumShape shape) {
    String name = typeName(shape);
    Map<String, String> values = shape.getEnumValues();
    writer.addInclude("<string>").addInclude("<string_view>");

    writeDocs(shape);
    writer.openBlock("class $L {", name);
    writer.write("public:").indent();

    writer.openBlock("enum class Value {");
    for (String memberName : values.keySet()) {
      writer.write("$L,", enumConstant(memberName));
    }
    writer.write("kUnknown,");
    writer.closeBlock("};");
    writer.write("");

    writer.write("$L() = default;", name);
    writer.write("$1L(Value value) : value_(value) {}  // NOLINT(*-explicit-*)", name);
    writer.write("");

    writer.write("/// Unknown wire values are preserved and reported as Value::kUnknown.");
    writer.openBlock("static $L FromString(std::string_view text) {", name);
    for (Map.Entry<String, String> entry : values.entrySet()) {
      writer.write(
          "if (text == $S) return $L(Value::$L);",
          entry.getValue(),
          name,
          enumConstant(entry.getKey()));
    }
    writer.write("$L result;", name);
    writer.write("result.unknown_ = std::string(text);");
    writer.write("return result;");
    writer.closeBlock("}");
    writer.write("");

    writer.write("Value value() const { return value_; }");
    writer.write("");
    writer.write("/// The wire text, including the original text of unknown values.");
    writer.openBlock("std::string_view ToString() const {");
    writer.openBlock("switch (value_) {");
    for (Map.Entry<String, String> entry : values.entrySet()) {
      writer.write("case Value::$L: return $S;", enumConstant(entry.getKey()), entry.getValue());
    }
    writer.write("case Value::kUnknown: return unknown_;");
    writer.closeBlock("}");
    writer.write("return unknown_;");
    writer.closeBlock("}");
    writer.write("");
    writer.write("friend bool operator==(const $1L&, const $1L&) = default;", name);
    writer.write("").dedent();

    writer.write("private:").indent();
    writer.write("Value value_ = Value::kUnknown;");
    writer.write("std::string unknown_;").dedent();
    writer.closeBlock("};");
    writer.write("");
  }

  void generateIntEnum(IntEnumShape shape) {
    String name = typeName(shape);
    writer.addInclude("<cstdint>");
    writeDocs(shape);
    writer.openBlock("enum class $L : std::int32_t {", name);
    for (Map.Entry<String, Integer> entry : shape.getEnumValues().entrySet()) {
      writer.write("$L = $L,", enumConstant(entry.getKey()), entry.getValue());
    }
    writer.closeBlock("};");
    writer.write("");
  }

  void generateUnion(UnionShape shape) {
    String name = typeName(shape);
    List<MemberShape> members = List.copyOf(shape.members());
    writer.addInclude("<utility>").addInclude("<variant>");

    writeDocs(shape);
    writer.openBlock("class $L {", name);
    writer.write("public:").indent();
    writer.write("$L() = default;", name);
    writer.write("");
    for (int i = 0; i < members.size(); ++i) {
      MemberShape member = members.get(i);
      Symbol type =
          context.symbolProvider().toSymbol(context.model().expectShape(member.getTarget()));
      writer.addIncludesFor(type);
      String memberName = symbols().toMemberName(member);
      int index = i + 1; // index 0 is the unset monostate
      writer.openBlock(
          "static $L From$L($L value) {", name, CaseUtils.toPascalCase(memberName), type.getName());
      writer.write("$L result;", name);
      writer.write("result.value_.emplace<$L>(std::move(value));", index);
      writer.write("return result;");
      writer.closeBlock("}");
      writer.write("bool is_$L() const { return value_.index() == $L; }", memberName, index);
      writer.write(
          "const $L& as_$L() const { return std::get<$L>(value_); }",
          type.getName(),
          memberName,
          index);
      writer.write("");
    }
    writer.write("/// True until one of the From* factories has been used.");
    writer.write("bool empty() const { return value_.index() == 0; }");
    writer.write("");
    writer.write("friend bool operator==(const $1L&, const $1L&) = default;", name);
    writer.write("").dedent();

    writer.write("private:").indent();
    StringBuilder variant = new StringBuilder("std::variant<std::monostate");
    for (MemberShape member : members) {
      variant
          .append(", ")
          .append(
              context
                  .symbolProvider()
                  .toSymbol(context.model().expectShape(member.getTarget()))
                  .getName());
    }
    variant.append(">");
    writer.write("$L value_;", variant).dedent();
    writer.closeBlock("};");
    writer.write("");
  }
}
