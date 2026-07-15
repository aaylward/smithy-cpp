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
   * Fails generation when two members of the same shape fold to one C++ name, or a member collides
   * with a reserved synthetic name. Enum-constant and union-factory naming lower-case, strip
   * separators, or PascalCase the member name, so distinct Smithy members ({@code fooBar} and
   * {@code foo_bar}, or a member literally named {@code unknown}) can produce a duplicate
   * enumerator/method that no longer compiles. Catching it here names both members and the fix
   * instead of surfacing a C++ redefinition error in the generated output.
   */
  static void requireDistinctNames(
      String kind,
      Object shapeId,
      java.util.LinkedHashMap<String, String> foldedByMember,
      java.util.Set<String> reserved) {
    java.util.Map<String, String> owner = new java.util.HashMap<>();
    for (var entry : foldedByMember.entrySet()) {
      String member = entry.getKey();
      String folded = entry.getValue();
      if (reserved.contains(folded)) {
        throw new software.amazon.smithy.codegen.core.CodegenException(
            "cpp-codegen: "
                + kind
                + " "
                + shapeId
                + " member '"
                + member
                + "' maps to the reserved generated name '"
                + folded
                + "'; rename the member");
      }
      String prior = owner.putIfAbsent(folded, member);
      if (prior != null) {
        throw new software.amazon.smithy.codegen.core.CodegenException(
            "cpp-codegen: "
                + kind
                + " "
                + shapeId
                + " members '"
                + prior
                + "' and '"
                + member
                + "' both map to the generated name '"
                + folded
                + "'; rename one so their generated names differ");
      }
    }
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
      Shape target = context.model().expectShape(member.getTarget());
      boolean emptyAggregate = target.isListShape() || target.isMapShape();
      if (MemberDefaults.populated(context.model(), member) && !emptyAggregate) {
        // @default: plain member initialized to the default value.
        writer.write(
            "$L $L = $L;",
            type.getName(),
            symbols().toMemberName(member),
            MemberDefaults.literal(context, member));
      } else {
        writer.write("$L $L{};", type.getName(), symbols().toMemberName(member));
      }
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
    java.util.LinkedHashMap<String, String> folded = new java.util.LinkedHashMap<>();
    for (String memberName : values.keySet()) {
      folded.put(memberName, enumConstant(memberName));
    }
    requireDistinctNames("enum", shape.getId(), folded, java.util.Set.of("kUnknown"));
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
    java.util.LinkedHashMap<String, String> folded = new java.util.LinkedHashMap<>();
    for (String memberName : shape.getEnumValues().keySet()) {
      folded.put(memberName, enumConstant(memberName));
    }
    requireDistinctNames("intEnum", shape.getId(), folded, java.util.Set.of());
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
    java.util.LinkedHashMap<String, String> folded = new java.util.LinkedHashMap<>();
    for (MemberShape member : members) {
      folded.put(
          member.getMemberName(), "From" + CaseUtils.toPascalCase(symbols().toMemberName(member)));
    }
    requireDistinctNames("union", shape.getId(), folded, java.util.Set.of());

    List<TaggedMember> tagged = new java.util.ArrayList<>();
    for (MemberShape member : members) {
      Symbol type =
          context.symbolProvider().toSymbol(context.model().expectShape(member.getTarget()));
      writer.addIncludesFor(type);
      tagged.add(new TaggedMember(symbols().toMemberName(member), type.getName()));
    }
    writeDocs(shape);
    emitTaggedVariant(
        writer,
        name,
        tagged,
        /* withFactories= */ true,
        "/// True until one of the From* factories has been used.",
        "/// Name of the engaged member, \"(empty)\" until a From* factory has run.",
        null);
  }

  /** One alternative of a tagged-variant class: accessor stem + C++ type name. */
  record TaggedMember(String memberName, String typeName) {}

  /**
   * The tagged-variant class shape shared by generated unions and per-operation error listings:
   * is_x/as_x/as_x_or_null accessors over {@code std::variant<std::monostate, ...>}, empty(),
   * case_name(), visit(), equality, and the contextful wrong-case guard (ADR-0009). Callers add
   * member-type includes; {@code extraPublic} (nullable) emits caller-specific members right after
   * the default constructor — the error listings' FromError factory.
   */
  static void emitTaggedVariant(
      CppWriter writer,
      String name,
      List<TaggedMember> members,
      boolean withFactories,
      String emptyDoc,
      String caseNameDoc,
      Runnable extraPublic) {
    writer.addInclude("<cstddef>").addInclude("<utility>").addInclude("<variant>");
    writer.addInclude("\"smithy/core/fatal.h\"");

    writer.openBlock("class $L {", name);
    writer.write("public:").indent();
    writer.write("$L() = default;", name);
    writer.write("");
    if (extraPublic != null) {
      extraPublic.run();
      writer.write("");
    }
    for (int i = 0; i < members.size(); ++i) {
      TaggedMember member = members.get(i);
      int index = i + 1; // index 0 is the unset monostate
      if (withFactories) {
        writer.openBlock(
            "static $L From$L($L value) {",
            name,
            CaseUtils.toPascalCase(member.memberName()),
            member.typeName());
        writer.write("$L result;", name);
        writer.write("result.value_.emplace<$L>(std::move(value));", index);
        writer.write("return result;");
        writer.closeBlock("}");
      }
      writer.write(
          "bool is_$L() const { return value_.index() == $L; }", member.memberName(), index);
      writer.openBlock("const $L& as_$L() const {", member.typeName(), member.memberName());
      writer.write("require_is($L, $S);", index, member.memberName());
      writer.write("return std::get<$L>(value_);", index);
      writer.closeBlock("}");
      writer.write("/// The engaged member, or nullptr when another member (or none) is set.");
      writer.write(
          "const $L* as_$L_or_null() const { return std::get_if<$L>(&value_); }",
          member.typeName(),
          member.memberName(),
          index);
      writer.write("");
    }
    writer.write(emptyDoc);
    writer.write("bool empty() const { return value_.index() == 0; }");
    writer.write("");
    String caseNames =
        java.util.stream.Stream.concat(
                java.util.stream.Stream.of("(empty)"),
                members.stream().map(TaggedMember::memberName))
            .map(CppLiterals::stringLiteral)
            .collect(java.util.stream.Collectors.joining(", "));
    writer.write(caseNameDoc);
    writer.openBlock("const char* case_name() const {");
    writer.write("static constexpr const char* kNames[] = {$L};", caseNames);
    writer.write("return kNames[value_.index()];");
    writer.closeBlock("}");
    writer.write("");
    writer.write("/// Applies `visitor` to the engaged member. The visitor must also accept");
    writer.write("/// std::monostate, which represents the empty state.");
    writer.write("template <typename Visitor>");
    writer.openBlock("decltype(auto) visit(Visitor&& visitor) const {");
    writer.write("return std::visit(std::forward<Visitor>(visitor), value_);");
    writer.closeBlock("}");
    writer.write("");
    writer.write("friend bool operator==(const $1L&, const $1L&) = default;", name);
    writer.write("").dedent();

    writer.write("private:").indent();
    writer.openBlock("void require_is(std::size_t index, const char* requested) const {");
    writer.openBlock("if (value_.index() != index) {");
    writer.write("smithy::internal::FatalWrongUnionAccess($S, requested, case_name());", name);
    writer.closeBlock("}");
    writer.closeBlock("}");
    writer.write("");
    String variant =
        members.stream()
            .map(TaggedMember::typeName)
            .collect(
                java.util.stream.Collectors.joining(
                    ", ", "std::variant<std::monostate, ", "> value_;"));
    writer.write("$L", variant).dedent();
    writer.closeBlock("};");
    writer.write("");
  }
}
