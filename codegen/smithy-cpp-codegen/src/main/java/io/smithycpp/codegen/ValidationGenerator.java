package io.smithycpp.codegen;

import java.math.BigDecimal;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import software.amazon.smithy.model.shapes.ListShape;
import software.amazon.smithy.model.shapes.MapShape;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.traits.LengthTrait;
import software.amazon.smithy.model.traits.PatternTrait;
import software.amazon.smithy.model.traits.RangeTrait;
import software.amazon.smithy.model.traits.SparseTrait;
import software.amazon.smithy.model.traits.Trait;
import software.amazon.smithy.model.traits.UniqueItemsTrait;

/**
 * Emits server-side constraint validation (smithy.framework#ValidationException semantics): one
 * {@code Validate<Shape>} function per transitively constrained aggregate shape, producing the
 * spec's exact failure messages and '/member/0'-style paths. Runs after parsing, before the
 * handler. Message formats follow the official {@code RestJsonValidation} conformance suite.
 */
final class ValidationGenerator {

  private final CppContext context;
  private final List<OperationShape> operations;

  /** Aggregate shapes that (transitively) carry constraints and need a Validate function. */
  private final Set<ShapeId> constrained = new LinkedHashSet<>();

  ValidationGenerator(CppContext context, List<OperationShape> operations) {
    this.context = context;
    this.operations = operations;
    for (OperationShape operation : operations) {
      collectConstrained(ProtocolSupport.inputShape(context, operation), new LinkedHashSet<>());
    }
  }

  /** Whether the operation's input needs validation at all. */
  boolean validates(OperationShape operation) {
    return constrained.contains(ProtocolSupport.inputShape(context, operation).getId());
  }

  /** Whether any operation input carries constraints (i.e. validators will be emitted). */
  boolean hasValidators() {
    return !constrained.isEmpty();
  }

  /** The Validate function name for the operation's input shape. */
  String validatorNameFor(OperationShape operation) {
    return validatorName(ProtocolSupport.inputShape(context, operation));
  }

  /** The suite's exact failure message for an absent required member. */
  static String memberMustNotBeNull(String path) {
    return "Value at '" + path + "' failed to satisfy constraint: Member must not be null";
  }

  private Shape target(MemberShape member) {
    return context.model().expectShape(member.getTarget());
  }

  private boolean memberHasConstraints(MemberShape member) {
    Shape target = target(member);
    return hasTrait(member, target, LengthTrait.class)
        || hasTrait(member, target, RangeTrait.class)
        || hasTrait(member, target, PatternTrait.class)
        || hasTrait(member, target, UniqueItemsTrait.class)
        || target.isEnumShape();
  }

  private static boolean hasTrait(MemberShape member, Shape target, Class<? extends Trait> t) {
    return member.hasTrait(t) || target.hasTrait(t);
  }

  private <T extends Trait> Optional<T> constraint(MemberShape member, Class<T> t) {
    // Member-applied traits override the target shape's.
    Optional<T> onMember = member.getTrait(t);
    return onMember.isPresent() ? onMember : target(member).getTrait(t);
  }

  private boolean collectConstrained(Shape shape, Set<ShapeId> visiting) {
    if (constrained.contains(shape.getId())) {
      return true;
    }
    if (!visiting.add(shape.getId())) {
      return false;
    }
    boolean any = false;
    for (MemberShape member : shape.members()) {
      if (memberHasConstraints(member)) {
        any = true;
      }
      Shape memberTarget = target(member);
      if (memberTarget.isStructureShape()
          || memberTarget.isUnionShape()
          || memberTarget.isListShape()
          || memberTarget.isMapShape()) {
        if (collectConstrained(memberTarget, visiting)) {
          any = true;
        }
      }
      // Map keys can carry constraints via the key member.
      if (memberTarget.isMapShape()) {
        MapShape map = memberTarget.asMapShape().orElseThrow();
        if (memberHasConstraints(map.getKey()) || memberHasConstraints(map.getValue())) {
          any = true;
          constrained.add(memberTarget.getId());
        }
      }
      if (memberTarget.isListShape()) {
        ListShape list = memberTarget.asListShape().orElseThrow();
        if (memberHasConstraints(list.getMember())) {
          any = true;
          constrained.add(memberTarget.getId());
        }
      }
    }
    if (any) {
      constrained.add(shape.getId());
    }
    return any;
  }

  /**
   * Emits the shared failure-recording helper; needed whenever validators are emitted or a
   * protocol's request parsing records required-member failures itself.
   */
  static void writeFailureHelper(CppWriter w) {
    w.addInclude("<string>");
    w.addInclude("<utility>");
    w.addInclude("<vector>");
    w.write("// Constraint validation (smithy.framework#ValidationException): messages");
    w.write("// and '/member' paths follow the official validation conformance suite.");
    w.openBlock(
        "void AddValidationFailure(std::vector<smithy::server::ValidationFailure>* failures, "
            + "std::string path, std::string message) {");
    w.write("failures->push_back({std::move(path), std::move(message)});");
    w.closeBlock("}");
    w.write("");
  }

  /**
   * Emits {@code ValidationErrorResponse(failures)}: the protocol's 400 ValidationException wire
   * shape ({@code message} summary + {@code fieldList}), built on the protocol's error helper.
   */
  static void writeValidationErrorResponse(
      CppWriter w, String errorFn, String errorCode, String errortypeHeader) {
    writeValidationErrorResponse(w, errorFn, errorCode, errortypeHeader, "", "");
  }

  /**
   * Variant threading extra context through ValidationErrorResponse into {@code errorFn}: {@code
   * extraParams} is appended to the signature and {@code extraArgs} to the {@code errorFn} call.
   * jsonRpc2 uses this to echo the request id into the error envelope.
   */
  static void writeValidationErrorResponse(
      CppWriter w,
      String errorFn,
      String errorCode,
      String errortypeHeader,
      String extraParams,
      String extraArgs) {
    w.addInclude("<cstddef>");
    w.addInclude("<string>");
    w.addInclude("<utility>");
    w.addInclude("<vector>");
    w.addInclude("\"smithy/core/document.h\"");
    w.openBlock(
        "smithy::http::HttpResponse ValidationErrorResponse("
            + "const std::vector<smithy::server::ValidationFailure>& failures$L) {",
        extraParams);
    w.write(
        "std::string summary = std::to_string(failures.size()) + \" validation error\" + "
            + "(failures.size() == 1 ? \"\" : \"s\") + \" detected. \";");
    w.write("smithy::DocumentList field_list;");
    w.openBlock("for (std::size_t i = 0; i < failures.size(); ++i) {");
    w.write("if (i > 0) summary += \"; \";");
    w.write("summary += failures[i].message;");
    w.write("smithy::DocumentMap field;");
    w.write("field.emplace(\"message\", smithy::Document(failures[i].message));");
    w.write("field.emplace(\"path\", smithy::Document(failures[i].path));");
    w.write("field_list.push_back(smithy::Document(std::move(field)));");
    w.closeBlock("}");
    w.write("smithy::DocumentMap body;");
    w.write("body.emplace(\"fieldList\", smithy::Document(std::move(field_list)));");
    w.write(
        "smithy::http::HttpResponse response = $L(400, $S, summary, std::move(body)$L);",
        errorFn,
        errorCode,
        extraArgs);
    if (!errortypeHeader.isEmpty()) {
      w.write("response.headers.Set($S, \"ValidationException\");", errortypeHeader);
    }
    w.write("return response;");
    w.closeBlock("}");
    w.write("");
  }

  /** Emits every Validate function (topological order via the serde ordering). */
  void writeValidators(CppWriter w) {
    if (constrained.isEmpty()) {
      return;
    }
    w.addInclude("<cstddef>");
    w.addInclude("<string>");
    w.addInclude("<vector>");
    SerdeGenerator ordering = new SerdeGenerator(context, false);
    // Constrained shapes on recursion cycles validate mutually; declare those
    // validators up front so definition order doesn't matter.
    RecursionIndex recursion = context.cppSymbols().recursion();
    boolean declared = false;
    for (Shape shape : ordering.serdeShapes()) {
      if (!constrained.contains(shape.getId()) || !recursion.inCycle(shape.getId())) {
        continue;
      }
      w.write(
          "void $L(const $L& value, const std::string& path, "
              + "std::vector<smithy::server::ValidationFailure>* failures);",
          validatorName(shape),
          context.cppSymbols().toSymbol(shape).getName());
      declared = true;
    }
    if (declared) {
      w.write("");
    }
    for (Shape shape : ordering.serdeShapes()) {
      if (!constrained.contains(shape.getId())) {
        continue;
      }
      writeValidator(w, shape);
    }
  }

  private String validatorName(Shape shape) {
    return "Validate" + SerdeCodeGen.serdeFunctionSuffix(context, shape);
  }

  private void writeValidator(CppWriter w, Shape shape) {
    String type = context.cppSymbols().toSymbol(shape).getName();
    w.openBlock(
        "void $L(const $L& value, const std::string& path, "
            + "std::vector<smithy::server::ValidationFailure>* failures) {",
        validatorName(shape),
        type);
    if (shape.isStructureShape() || shape.isUnionShape()) {
      for (MemberShape member : shape.members()) {
        writeStructureMemberChecks(w, shape, member);
      }
    } else if (shape.isListShape()) {
      ListShape list = shape.asListShape().orElseThrow();
      MemberShape element = list.getMember();
      boolean sparse = shape.hasTrait(SparseTrait.class);
      w.openBlock("for (std::size_t i = 0; i < value.size(); ++i) {");
      w.write("const std::string item_path = path + \"/\" + std::to_string(i);");
      String item = sparse ? "(*value[i])" : "value[i]";
      if (sparse) {
        // Sparse list elements are std::optional; null entries are valid.
        w.openBlock("if (value[i].has_value()) {");
      }
      writeValueChecks(w, element, item, "item_path");
      writeRecursion(w, element, item, "item_path");
      if (sparse) {
        w.closeBlock("}");
      }
      w.closeBlock("}");
    } else if (shape.isMapShape()) {
      MapShape map = shape.asMapShape().orElseThrow();
      boolean sparse = shape.hasTrait(SparseTrait.class);
      w.openBlock("for (const auto& [key, item] : value) {");
      // Key violations are reported at the map's own path. Keys are always
      // std::string in the generated map type, so enum keys check via FromString.
      writeValueChecks(w, map.getKey(), "key", "path", /* enumAsRawString= */ true);
      w.write("const std::string item_path = path + \"/\" + key;");
      String item = sparse ? "(*item)" : "item";
      if (sparse) {
        // Sparse map values are std::optional; null entries are valid.
        w.openBlock("if (item.has_value()) {");
      }
      writeValueChecks(w, map.getValue(), item, "item_path");
      writeRecursion(w, map.getValue(), item, "item_path");
      if (sparse) {
        w.closeBlock("}");
      }
      w.closeBlock("}");
    }
    w.closeBlock("}");
    w.write("");
  }

  private void writeStructureMemberChecks(CppWriter w, Shape shape, MemberShape member) {
    Shape memberTarget = target(member);
    boolean needsChecks =
        memberHasConstraints(member) || constrained.contains(memberTarget.getId());
    if (!needsChecks) {
      return;
    }
    String field;
    boolean guarded;
    if (shape.isUnionShape()) {
      String name = context.cppSymbols().toMemberName(member);
      w.openBlock("if (value.is_$L()) {", name);
      field = "value.as_" + name + "()";
      guarded = true;
    } else if (!MemberDefaults.plain(context.model(), member)) {
      String rawField = "value." + context.cppSymbols().toMemberName(member);
      w.openBlock("if ($L.has_value()) {", rawField);
      field = "(*" + rawField + ")";
      guarded = true;
    } else {
      // Own scope: consecutive required members each declare member_path.
      w.openBlock("{");
      field = "value." + context.cppSymbols().toMemberName(member);
      guarded = true;
    }
    String pathExpr = "path + \"/" + member.getMemberName() + "\"";
    w.write("const std::string member_path = $L;", pathExpr);
    writeValueChecks(w, member, field, "member_path");
    writeRecursion(w, member, field, "member_path");
    if (guarded) {
      w.closeBlock("}");
    }
  }

  private void writeRecursion(CppWriter w, MemberShape member, String valueExpr, String pathVar) {
    Shape memberTarget = target(member);
    if (constrained.contains(memberTarget.getId())) {
      w.write("$L($L, $L, failures);", validatorName(memberTarget), valueExpr, pathVar);
    }
  }

  private void writeValueChecks(CppWriter w, MemberShape member, String valueExpr, String pathVar) {
    writeValueChecks(w, member, valueExpr, pathVar, /* enumAsRawString= */ false);
  }

  /**
   * The member-level constraint checks for one value expression. {@code enumAsRawString} marks
   * positions (map keys) where an enum-targeted value is a plain {@code std::string} in C++.
   */
  private void writeValueChecks(
      CppWriter w, MemberShape member, String valueExpr, String pathVar, boolean enumAsRawString) {
    Shape memberTarget = target(member);
    constraint(member, LengthTrait.class)
        .ifPresent(
            length ->
                writeLengthCheck(w, memberTarget, length, valueExpr, pathVar, enumAsRawString));
    constraint(member, RangeTrait.class)
        .ifPresent(range -> writeRangeCheck(w, range, valueExpr, pathVar));
    constraint(member, PatternTrait.class)
        .ifPresent(pattern -> writePatternCheck(w, pattern, valueExpr, pathVar));
    if (constraint(member, UniqueItemsTrait.class).isPresent()) {
      writeUniqueItemsCheck(w, valueExpr, pathVar);
    }
    if (memberTarget.isEnumShape()) {
      String checked =
          enumAsRawString
              ? context.cppSymbols().toSymbol(memberTarget).getName()
                  + "::FromString("
                  + valueExpr
                  + ")"
              : valueExpr;
      writeEnumCheck(w, memberTarget, checked, pathVar);
    }
  }

  private static String plainNumber(BigDecimal value) {
    return value.stripTrailingZeros().toPlainString();
  }

  private void writeLengthCheck(
      CppWriter w,
      Shape target,
      LengthTrait length,
      String valueExpr,
      String pathVar,
      boolean enumAsRawString) {
    String lengthExpr =
        switch (target.getType()) {
          case STRING -> "smithy::Utf8CodePointCount(" + valueExpr + ")";
          case ENUM ->
              enumAsRawString
                  ? "smithy::Utf8CodePointCount(" + valueExpr + ")"
                  : "smithy::Utf8CodePointCount(" + valueExpr + ".ToString())";
          default -> valueExpr + ".size()";
        };
    if (target.getType() == software.amazon.smithy.model.shapes.ShapeType.STRING
        || target.getType() == software.amazon.smithy.model.shapes.ShapeType.ENUM) {
      w.addInclude("\"smithy/core/text.h\"");
    }
    Optional<Long> min = length.getMin();
    Optional<Long> max = length.getMax();
    String condition;
    String constraintText;
    if (min.isPresent() && max.isPresent()) {
      condition = "member_length < " + min.get() + "ULL || member_length > " + max.get() + "ULL";
      constraintText =
          "Member must have length between " + min.get() + " and " + max.get() + ", inclusive";
    } else if (min.isPresent()) {
      condition = "member_length < " + min.get() + "ULL";
      constraintText = "Member must have length greater than or equal to " + min.get();
    } else {
      condition = "member_length > " + max.get() + "ULL";
      constraintText = "Member must have length less than or equal to " + max.get();
    }
    w.openBlock("{");
    w.write("const std::size_t member_length = $L;", lengthExpr);
    w.openBlock("if ($L) {", condition);
    w.write(
        "AddValidationFailure(failures, $L, \"Value with length \" + "
            + "std::to_string(member_length) + \" at '\" + $L + \"' failed to satisfy "
            + "constraint: $L\");",
        pathVar,
        pathVar,
        constraintText);
    w.closeBlock("}");
    w.closeBlock("}");
  }

  private void writeRangeCheck(CppWriter w, RangeTrait range, String valueExpr, String pathVar) {
    Optional<BigDecimal> min = range.getMin();
    Optional<BigDecimal> max = range.getMax();
    String condition;
    String constraintText;
    if (min.isPresent() && max.isPresent()) {
      condition =
          valueExpr
              + " < "
              + plainNumber(min.get())
              + " || "
              + valueExpr
              + " > "
              + plainNumber(max.get());
      constraintText =
          "Member must be between "
              + plainNumber(min.get())
              + " and "
              + plainNumber(max.get())
              + ", inclusive";
    } else if (min.isPresent()) {
      condition = valueExpr + " < " + plainNumber(min.get());
      constraintText = "Member must be greater than or equal to " + plainNumber(min.get());
    } else {
      condition = valueExpr + " > " + plainNumber(max.get());
      constraintText = "Member must be less than or equal to " + plainNumber(max.get());
    }
    w.openBlock("if ($L) {", condition);
    w.write(
        "AddValidationFailure(failures, $L, \"Value at '\" + $L + \"' failed to satisfy "
            + "constraint: $L\");",
        pathVar,
        pathVar,
        constraintText);
    w.closeBlock("}");
  }

  private int patternCounter = 0;

  private void writePatternCheck(
      CppWriter w, PatternTrait pattern, String valueExpr, String pathVar) {
    String variable = "kPattern" + patternCounter++;
    w.addInclude("<regex>");
    // Raw string literal keeps the regex byte-exact; the failure message needs
    // C++ escaping instead.
    w.write(
        "static const std::regex $L{R\"__smithy($L)__smithy\", std::regex::ECMAScript};",
        variable,
        pattern.getValue());
    w.openBlock("if (!std::regex_search($L, $L)) {", valueExpr, variable);
    w.write(
        "AddValidationFailure(failures, $L, \"Value at '\" + $L + \"' failed to satisfy "
            + "constraint: Member must satisfy regular expression pattern: \" + "
            + "std::string($L));",
        pathVar,
        pathVar,
        CppLiterals.stringLiteral(pattern.getValue()));
    w.closeBlock("}");
  }

  private void writeUniqueItemsCheck(CppWriter w, String valueExpr, String pathVar) {
    w.openBlock("{");
    w.write("bool unique = true;");
    w.openBlock("for (std::size_t i = 0; i < $L.size() && unique; ++i) {", valueExpr);
    w.openBlock("for (std::size_t j = i + 1; j < $L.size(); ++j) {", valueExpr);
    w.write("if ($L[i] == $L[j]) { unique = false; break; }", valueExpr, valueExpr);
    w.closeBlock("}");
    w.closeBlock("}");
    w.openBlock("if (!unique) {");
    w.write(
        "AddValidationFailure(failures, $L, \"Value at '\" + $L + \"' failed to satisfy "
            + "constraint: Member must have unique values\");",
        pathVar,
        pathVar);
    w.closeBlock("}");
    w.closeBlock("}");
  }

  private void writeEnumCheck(CppWriter w, Shape target, String valueExpr, String pathVar) {
    // @internal enum members stay valid on the wire but are omitted from the
    // advertised value set (the suite pins this, incl. legacy @enum internal tags).
    var values =
        target.asEnumShape().orElseThrow().members().stream()
            .filter(
                member -> !member.hasTrait(software.amazon.smithy.model.traits.InternalTrait.class))
            .map(
                member ->
                    member
                        .expectTrait(software.amazon.smithy.model.traits.EnumValueTrait.class)
                        .expectStringValue())
            .toList();
    String set = String.join(", ", values);
    String type = context.cppSymbols().toSymbol(target).getName();
    w.openBlock("if ($L.value() == $L::Value::kUnknown) {", valueExpr, type);
    w.write(
        "AddValidationFailure(failures, $L, \"Value at '\" + $L + \"' failed to satisfy "
            + "constraint: Member must satisfy enum value set: [$L]\");",
        pathVar,
        pathVar,
        set);
    w.closeBlock("}");
  }
}
