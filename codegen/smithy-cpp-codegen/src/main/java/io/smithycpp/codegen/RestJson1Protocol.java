package io.smithycpp.codegen;

import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.knowledge.HttpBinding;
import software.amazon.smithy.model.knowledge.HttpBindingIndex;
import software.amazon.smithy.model.pattern.SmithyPattern;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.HttpTrait;

/** restJson1 client bindings: HTTP method/URI from @http, labels, query, headers, JSON bodies. */
final class RestJson1Protocol implements ProtocolGenerator {

  @Override
  public String name() {
    return "restJson1";
  }

  @Override
  public software.amazon.smithy.model.shapes.ShapeId traitId() {
    return software.amazon.smithy.aws.traits.protocols.RestJson1Trait.ID;
  }

  @Override
  public String contentType() {
    return "application/json";
  }

  @Override
  public boolean usesJsonName() {
    return true;
  }

  @Override
  public void writeErrorDetailPatches(CppWriter w, CppContext context, StructureShape error) {
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    for (HttpBinding binding : new TreeMap<>(index.getResponseBindings(error)).values()) {
      if (binding.getLocation() != HttpBinding.Location.HEADER) {
        continue;
      }
      MemberShape member = binding.getMember();
      Shape target = context.model().expectShape(member.getTarget());
      // Outcome-parsing kinds (timestamps, media-type strings) and lists are
      // not patched into error details yet; the exclusion list documents any
      // affected conformance cases.
      String expr =
          switch (target.getType()) {
            case STRING ->
                target.hasTrait(software.amazon.smithy.model.traits.MediaTypeTrait.class)
                    ? null
                    : "*header_value";
            case ENUM ->
                context.cppSymbols().toSymbol(target).getName() + "::FromString(*header_value)";
            case BYTE, SHORT, INTEGER, LONG, INT_ENUM ->
                "static_cast<"
                    + context.cppSymbols().toSymbol(target).getName()
                    + ">(ParseHeaderInt64(*header_value))";
            case FLOAT -> "static_cast<float>(ParseHeaderDouble(*header_value))";
            case DOUBLE -> "ParseHeaderDouble(*header_value)";
            case BOOLEAN -> "(*header_value == \"true\")";
            default -> null;
          };
      if (expr == null) {
        continue;
      }
      w.openBlock(
          "if (const auto header_value = response.headers.Get($S); header_value.has_value()) {",
          binding.getLocationName());
      w.write("detail->$L = $L;", context.cppSymbols().toMemberName(member), expr);
      w.closeBlock("}");
    }
  }

  @Override
  public List<String> runtimeDeps() {
    return List.of(":json");
  }

  @Override
  public List<String> clientIncludes() {
    return List.of(
        "\"smithy/json/json.h\"",
        "\"smithy/core/base64.h\"",
        "\"smithy/http/headers.h\"",
        "<cstdint>",
        "<cstdlib>",
        "<limits>");
  }

  @Override
  public void writeClientHelpers(CppWriter w, CppContext context) {
    ProtocolSupport.writeErrorSupport(
        w, "auto doc = smithy::json::Decode(response.body);", /* errorTypeHeader= */ true);
    w.write("// Response @httpHeader value parsing ([[maybe_unused]]: emitted for every");
    w.write("// client; individual services may bind no numeric/float headers).");
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

  @Override
  public void writeOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    HttpTrait http =
        operation
            .getTrait(HttpTrait.class)
            .orElseThrow(
                () ->
                    new CodegenException(
                        "cpp-codegen: restJson1 operation missing @http: " + operation.getId()));
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StructureShape output = ProtocolSupport.outputShape(context, operation);
    SerdeCodeGen serde = new SerdeCodeGen(context);

    Map<String, HttpBinding> labels = new TreeMap<>();
    Map<String, HttpBinding> queries = new TreeMap<>();
    Map<String, HttpBinding> headers = new TreeMap<>();
    List<HttpBinding> body = new java.util.ArrayList<>();
    HttpBinding queryParams = null;
    for (HttpBinding binding : index.getRequestBindings(operation).values()) {
      switch (binding.getLocation()) {
        case LABEL -> labels.put(binding.getLocationName(), binding);
        case QUERY -> queries.put(binding.getLocationName(), binding);
        case QUERY_PARAMS -> queryParams = binding;
        case HEADER -> headers.put(binding.getLocationName(), binding);
        case DOCUMENT -> body.add(binding);
        default ->
            throw new CodegenException(
                "cpp-codegen: restJson1 input binding "
                    + binding.getLocation()
                    + " is not supported yet ("
                    + operation.getId()
                    + ")");
      }
    }
    Map<String, HttpBinding> responseHeaders = new TreeMap<>();
    List<HttpBinding> responseBody = new java.util.ArrayList<>();
    for (HttpBinding binding : index.getResponseBindings(operation).values()) {
      switch (binding.getLocation()) {
        case DOCUMENT -> responseBody.add(binding);
        case HEADER -> {
          if (binding.getMember().isRequired()) {
            throw new CodegenException(
                "cpp-codegen: @required response @httpHeader members are not supported yet ("
                    + operation.getId()
                    + ")");
          }
          responseHeaders.put(binding.getLocationName(), binding);
        }
        default ->
            throw new CodegenException(
                "cpp-codegen: restJson1 output binding "
                    + binding.getLocation()
                    + " is not supported yet ("
                    + operation.getId()
                    + ")");
      }
    }

    String in =
        ProtocolSupport.prepareIdempotencyTokens(
            w, context, input, context.cppSymbols().toSymbol(input).getName());

    // Target: path segments in pattern order, then query.
    w.write("std::string target = path_prefix_;");
    for (SmithyPattern.Segment segment : http.getUri().getSegments()) {
      if (!segment.isLabel()) {
        w.write("target += \"/$L\";", segment.getContent());
        continue;
      }
      HttpBinding binding = labels.get(segment.getContent());
      if (binding == null) {
        throw new CodegenException(
            "cpp-codegen: no @httpLabel member for {"
                + segment.getContent()
                + "} in "
                + operation.getId());
      }
      String value =
          ProtocolSupport.toStringExpression(
              context,
              binding.getMember(),
              in + "." + context.cppSymbols().toMemberName(binding.getMember()),
              serde.timestampFormat(binding.getMember(), "smithy::TimestampFormat::kDateTime"));
      String encoder = segment.isGreedyLabel() ? "EncodeGreedyPathSegment" : "EncodePathSegment";
      w.write("target += \"/\";");
      w.write("target += smithy::http::$L($L);", encoder, value);
    }
    boolean hasQuery =
        !queries.isEmpty() || queryParams != null || !http.getUri().getQueryLiterals().isEmpty();
    if (hasQuery) {
      w.write("smithy::http::QueryString query;");
      for (Map.Entry<String, String> literal :
          new TreeMap<>(http.getUri().getQueryLiterals()).entrySet()) {
        if (literal.getValue().isEmpty()) {
          w.write("query.AddFlag($S);", literal.getKey());
        } else {
          w.write("query.Add($S, $S);", literal.getKey(), literal.getValue());
        }
      }
      for (HttpBinding binding : queries.values()) {
        writeQueryBinding(w, context, serde, binding, in);
      }
      if (queryParams != null) {
        writeQueryParamsBinding(w, context, queryParams, in);
      }
      w.write("target += query.ToString();");
    }

    w.write("smithy::http::HttpRequest request;");
    w.write("request.method = $S;", http.getMethod());
    w.write("request.target = std::move(target);");
    for (HttpBinding binding : headers.values()) {
      writeHeaderBinding(w, context, serde, binding, in);
    }

    if (!body.isEmpty()) {
      w.write("smithy::DocumentMap body_map;");
      for (HttpBinding binding : body) {
        MemberShape member = binding.getMember();
        String field = in + "." + context.cppSymbols().toMemberName(member);
        String wireName =
            member
                .getTrait(software.amazon.smithy.model.traits.JsonNameTrait.class)
                .map(software.amazon.smithy.model.traits.JsonNameTrait::getValue)
                .orElse(member.getMemberName());
        if (member.isRequired()) {
          w.write("body_map.emplace($S, $L);", wireName, serde.serializeExpression(member, field));
        } else {
          w.openBlock("if ($L.has_value()) {", field);
          w.write(
              "body_map.emplace($S, $L);",
              wireName,
              serde.serializeExpression(member, "(*" + field + ")"));
          w.closeBlock("}");
        }
      }
      w.write("request.body = smithy::json::Encode(smithy::Document(std::move(body_map)));");
      w.write("request.headers.Set(\"content-type\", \"application/json\");");
    }

    w.write("auto response = Send(std::move(request));");
    w.write("if (!response) return std::move(response).error();");
    w.write(
        "if (response->status != $L) return $L;",
        http.getCode(),
        ProtocolSupport.errorExpression(context, service, operation));

    String outType = context.cppSymbols().toSymbol(output).getName();
    if (output.members().isEmpty()) {
      w.write("return $L{};", outType);
      return;
    }
    boolean allOptional = output.members().stream().noneMatch(MemberShape::isRequired);
    if (responseHeaders.isEmpty()) {
      if (allOptional) {
        w.write("if (response->body.empty()) return $L{};", outType);
      }
      w.write("auto body_doc = smithy::json::Decode(response->body);");
      w.write("if (!body_doc) return std::move(body_doc).error();");
      w.write(
          "return Deserialize$L(*body_doc);", SerdeCodeGen.serdeFunctionSuffix(context, output));
      return;
    }
    w.write("$L out{};", outType);
    if (!responseBody.isEmpty()) {
      // Header-bound members are always optional (checked above), so a
      // required member here means a required body member: parse regardless.
      if (allOptional) {
        w.openBlock("if (!response->body.empty()) {");
      }
      w.write("auto body_doc = smithy::json::Decode(response->body);");
      w.write("if (!body_doc) return std::move(body_doc).error();");
      w.write(
          "auto parsed = Deserialize$L(*body_doc);",
          SerdeCodeGen.serdeFunctionSuffix(context, output));
      w.write("if (!parsed) return std::move(parsed).error();");
      w.write("out = *std::move(parsed);");
      if (allOptional) {
        w.closeBlock("}");
      }
    }
    for (HttpBinding binding : responseHeaders.values()) {
      writeResponseHeaderBinding(w, context, serde, binding);
    }
    w.write("return out;");
  }

  /** Patches one response @httpHeader member into {@code out} (member is always optional). */
  private void writeResponseHeaderBinding(
      CppWriter w, CppContext context, SerdeCodeGen serde, HttpBinding binding) {
    MemberShape member = binding.getMember();
    Shape target = context.model().expectShape(member.getTarget());
    String field = "out." + context.cppSymbols().toMemberName(member);
    w.openBlock(
        "if (const auto header_value = response->headers.Get($S); header_value.has_value()) {",
        binding.getLocationName());
    if (target.isListShape()) {
      var list = target.asListShape().orElseThrow();
      MemberShape element = list.getMember();
      w.write("$L items;", context.cppSymbols().toSymbol(list).getName());
      String tsFormat = serde.timestampFormat(element, "smithy::TimestampFormat::kHttpDate");
      String splitter =
          context.model().expectShape(element.getTarget()).isTimestampShape()
                  && tsFormat.equals("smithy::TimestampFormat::kHttpDate")
              ? "SplitHttpDateHeaderValues"
              : "SplitHeaderListValues";
      w.openBlock("for (const std::string& part : smithy::http::$L(*header_value)) {", splitter);
      writeParsedHeaderValue(
          w,
          context,
          serde,
          element,
          "part",
          "items.push_back",
          serde.timestampFormat(element, "smithy::TimestampFormat::kHttpDate"));
      w.closeBlock("}");
      w.write("$L = std::move(items);", field);
    } else {
      writeParsedHeaderValue(
          w,
          context,
          serde,
          member,
          "(*header_value)",
          field + " = ",
          serde.timestampFormat(member, "smithy::TimestampFormat::kHttpDate"));
    }
    w.closeBlock("}");
  }

  /**
   * Emits `<sink>(parsed)` / `<sink> parsed;` for one header text value. Timestamps and media-type
   * strings parse through Outcomes, so those emit a statement pair instead of one expression.
   */
  private void writeParsedHeaderValue(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      MemberShape member,
      String valueExpr,
      String sink,
      String timestampFormat) {
    Shape target = context.model().expectShape(member.getTarget());
    boolean call = !sink.endsWith("= ");
    String open = call ? sink + "(" : sink;
    String close = call ? ");" : ";";
    switch (target.getType()) {
      case TIMESTAMP -> {
        w.write("auto parsed_ts = smithy::Timestamp::Parse($L, $L);", valueExpr, timestampFormat);
        w.write("if (!parsed_ts) return std::move(parsed_ts).error();");
        w.write("$L*std::move(parsed_ts)$L", open, close);
      }
      case STRING -> {
        if (target.hasTrait(software.amazon.smithy.model.traits.MediaTypeTrait.class)) {
          // restJson1: string headers with @mediaType are base64 encoded.
          w.write("auto decoded = smithy::Base64Decode($L);", valueExpr);
          w.write("if (!decoded) return std::move(decoded).error();");
          w.write("$Ldecoded->ToString()$L", open, close);
        } else {
          w.write("$L$L$L", open, valueExpr, close);
        }
      }
      case ENUM ->
          w.write(
              "$L$L::FromString($L)$L",
              open,
              context.cppSymbols().toSymbol(target).getName(),
              valueExpr,
              close);
      case BYTE, SHORT, INTEGER, LONG, INT_ENUM ->
          w.write(
              "$Lstatic_cast<$L>(ParseHeaderInt64($L))$L",
              open,
              context.cppSymbols().toSymbol(target).getName(),
              valueExpr,
              close);
      case FLOAT ->
          w.write("$Lstatic_cast<float>(ParseHeaderDouble($L))$L", open, valueExpr, close);
      case DOUBLE -> w.write("$LParseHeaderDouble($L)$L", open, valueExpr, close);
      case BOOLEAN -> w.write("$L($L == \"true\")$L", open, valueExpr, close);
      default ->
          throw new CodegenException(
              "cpp-codegen: response @httpHeader target "
                  + target.getId()
                  + " is not supported yet");
    }
  }

  /**
   * @httpQuery member: simple value, or a list of simple values added one entry per element.
   */
  private void writeQueryBinding(
      CppWriter w, CppContext context, SerdeCodeGen serde, HttpBinding binding, String in) {
    MemberShape member = binding.getMember();
    Shape target = context.model().expectShape(member.getTarget());
    String field = in + "." + context.cppSymbols().toMemberName(member);
    String name = binding.getLocationName();
    String access = member.isRequired() ? field : "(*" + field + ")";
    if (!member.isRequired()) {
      w.openBlock("if ($L.has_value()) {", field);
    }
    if (target.isListShape()) {
      MemberShape element = target.asListShape().orElseThrow().getMember();
      w.openBlock("for (const auto& item : $L) {", access);
      w.write(
          "query.Add($S, $L);",
          name,
          ProtocolSupport.toStringExpression(
              context,
              element,
              "item",
              serde.timestampFormat(element, "smithy::TimestampFormat::kDateTime")));
      w.closeBlock("}");
    } else {
      w.write(
          "query.Add($S, $L);",
          name,
          ProtocolSupport.toStringExpression(
              context,
              member,
              access,
              serde.timestampFormat(member, "smithy::TimestampFormat::kDateTime")));
    }
    if (!member.isRequired()) {
      w.closeBlock("}");
    }
  }

  /**
   * @httpQueryParams map: bound query members and literals take precedence (query.Has).
   */
  private void writeQueryParamsBinding(
      CppWriter w, CppContext context, HttpBinding binding, String in) {
    MemberShape member = binding.getMember();
    var map = context.model().expectShape(member.getTarget()).asMapShape().orElseThrow();
    Shape valueTarget = context.model().expectShape(map.getValue().getTarget());
    String field = in + "." + context.cppSymbols().toMemberName(member);
    String access = member.isRequired() ? field : "(*" + field + ")";
    if (!member.isRequired()) {
      w.openBlock("if ($L.has_value()) {", field);
    }
    w.openBlock("for (const auto& [key, value] : $L) {", access);
    w.write("if (query.Has(key)) continue;");
    if (valueTarget.isListShape()) {
      w.openBlock("for (const auto& item : value) {");
      w.write("query.Add(key, item);");
      w.closeBlock("}");
    } else {
      w.write("query.Add(key, value);");
    }
    w.closeBlock("}");
    if (!member.isRequired()) {
      w.closeBlock("}");
    }
  }

  /**
   * @httpHeader member: simple value, or a list joined with ", " per the HTTP binding spec.
   */
  private void writeHeaderBinding(
      CppWriter w, CppContext context, SerdeCodeGen serde, HttpBinding binding, String in) {
    MemberShape member = binding.getMember();
    Shape target = context.model().expectShape(member.getTarget());
    String field = in + "." + context.cppSymbols().toMemberName(member);
    String name = binding.getLocationName();
    String access = member.isRequired() ? field : "(*" + field + ")";
    if (!member.isRequired()) {
      w.openBlock("if ($L.has_value()) {", field);
    }
    if (target.isListShape()) {
      MemberShape element = target.asListShape().orElseThrow().getMember();
      w.openBlock("{");
      w.write("std::string joined;");
      w.openBlock("for (const auto& item : $L) {", access);
      w.write("if (!joined.empty()) joined += \", \";");
      w.write(
          "joined += $L;",
          ProtocolSupport.toStringExpression(
              context,
              element,
              "item",
              serde.timestampFormat(element, "smithy::TimestampFormat::kHttpDate")));
      w.closeBlock("}");
      w.write("request.headers.Set($S, joined);", name);
      w.closeBlock("}");
    } else {
      String expr =
          ProtocolSupport.toStringExpression(
              context,
              member,
              access,
              serde.timestampFormat(member, "smithy::TimestampFormat::kHttpDate"));
      if (target.isStringShape()
          && target.hasTrait(software.amazon.smithy.model.traits.MediaTypeTrait.class)) {
        // restJson1: string headers with @mediaType are base64 encoded.
        expr = "smithy::Base64Encode(smithy::Blob::FromString(" + expr + "))";
      }
      w.write("request.headers.Set($S, $L);", name, expr);
    }
    if (!member.isRequired()) {
      w.closeBlock("}");
    }
  }
}
