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

  /** Set up by writeServerHelpers (always called before the routes are emitted). */
  private ValidationGenerator validation;

  /** Whether the service emits ValidationErrorResponse (constraints or top-level @required). */
  private boolean emitsValidation;

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
    ProtocolSupport.writeNumericParseHelpers(w);
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
    HttpBinding responseCode = null;
    for (HttpBinding binding : index.getResponseBindings(operation).values()) {
      switch (binding.getLocation()) {
        case DOCUMENT -> responseBody.add(binding);
        case RESPONSE_CODE -> responseCode = binding;
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
    if (responseCode != null) {
      // The service chooses the (2xx) status at runtime via @httpResponseCode.
      w.write(
          "if (response->status < 200 || response->status > 299) return $L;",
          ProtocolSupport.errorExpression(context, service, operation));
    } else {
      w.write(
          "if (response->status != $L) return $L;",
          http.getCode(),
          ProtocolSupport.errorExpression(context, service, operation));
    }

    String outType = context.cppSymbols().toSymbol(output).getName();
    if (output.members().isEmpty()) {
      w.write("return $L{};", outType);
      return;
    }
    boolean allOptional = output.members().stream().noneMatch(MemberShape::isRequired);
    if (responseHeaders.isEmpty() && responseCode == null) {
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
    if (responseCode != null) {
      MemberShape codeMember = responseCode.getMember();
      String type =
          context
              .cppSymbols()
              .toSymbol(context.model().expectShape(codeMember.getTarget()))
              .getName();
      w.write(
          "out.$L = static_cast<$L>(response->status);",
          context.cppSymbols().toMemberName(codeMember),
          type);
    }
    w.write("return out;");
  }

  /** Patches one response @httpHeader member into {@code out} (member is always optional). */
  private void writeResponseHeaderBinding(
      CppWriter w, CppContext context, SerdeCodeGen serde, HttpBinding binding) {
    writeHeaderReadBinding(w, context, serde, binding, "response->headers", "out.");
  }

  /**
   * Reads one @httpHeader binding from {@code headersExpr} into {@code targetPrefix}<member>
   * (client response headers and server request headers share this shape; failures return
   * smithy::Error, so it must run inside an Outcome-returning function).
   */
  void writeHeaderReadBinding(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      HttpBinding binding,
      String headersExpr,
      String targetPrefix) {
    MemberShape member = binding.getMember();
    Shape target = context.model().expectShape(member.getTarget());
    String field = targetPrefix + context.cppSymbols().toMemberName(member);
    w.openBlock(
        "if (const auto header_value = $L.Get($S); header_value.has_value()) {",
        headersExpr,
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
    writeHeaderWriteBinding(w, context, serde, binding, in, "request.headers");
  }

  /** Writes one @httpHeader binding from {@code owner}.<member> into {@code headersExpr}. */
  private void writeHeaderWriteBinding(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      HttpBinding binding,
      String owner,
      String headersExpr) {
    MemberShape member = binding.getMember();
    Shape target = context.model().expectShape(member.getTarget());
    String field = owner + "." + context.cppSymbols().toMemberName(member);
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
      w.write("$L.Set($S, joined);", headersExpr, name);
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
      w.write("$L.Set($S, $L);", headersExpr, name, expr);
    }
    if (!member.isRequired()) {
      w.closeBlock("}");
    }
  }

  // ---------------------------------------------------------------------
  // Server emission
  // ---------------------------------------------------------------------

  @Override
  public List<String> serverIncludes() {
    return List.of(
        "\"smithy/json/json.h\"",
        "\"smithy/core/base64.h\"",
        "\"smithy/core/document_serde.h\"",
        "\"smithy/core/blob.h\"",
        "\"smithy/http/headers.h\"",
        "<cstdint>",
        "<cstdlib>",
        "<limits>");
  }

  @Override
  public void writeServerHelpers(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    SerdeCodeGen serde = new SerdeCodeGen(context);
    ProtocolSupport.writeNumericParseHelpers(w);
    w.openBlock(
        "smithy::http::HttpResponse JsonError(int status, const std::string& code, "
            + "const std::string& message, smithy::DocumentMap body) {");
    w.write("if (!code.empty()) body.insert_or_assign(\"__type\", smithy::Document(code));");
    w.write("if (!message.empty()) body.insert_or_assign(\"message\", smithy::Document(message));");
    w.write("smithy::http::HttpResponse response;");
    w.write("response.status = status;");
    w.write("response.headers.Set(\"content-type\", \"application/json\");");
    w.write("response.body = smithy::json::Encode(smithy::Document(std::move(body)));");
    w.write("return response;");
    w.closeBlock("}");
    w.write("");
    ProtocolSupport.writeServerErrorToResponse(
        w, context, service, operations, "JsonError", /* errortypeHeader= */ true);
    validation = new ValidationGenerator(context, operations);
    emitsValidation = validation.hasValidators() || anyTopLevelRequired(context, operations);
    if (emitsValidation) {
      ValidationGenerator.writeFailureHelper(w);
      validation.writeValidators(w);
      ValidationGenerator.writeValidationErrorResponse(
          w, "JsonError", "", /* errortypeHeader= */ true);
    }
    for (OperationShape operation : operations) {
      writeParseInputFunction(w, context, serde, operation);
      writeSerializeResponseFunction(w, context, serde, operation);
    }
  }

  /**
   * Whether any operation has a required top-level body/query/header member: those absences are
   * ValidationException failures ("Member must not be null"), not serialization errors. Absent
   * labels never route here, and nested required members stay serde-strict.
   */
  private static boolean anyTopLevelRequired(CppContext context, List<OperationShape> operations) {
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    for (OperationShape operation : operations) {
      for (HttpBinding binding : index.getRequestBindings(operation).values()) {
        boolean location =
            switch (binding.getLocation()) {
              case DOCUMENT, QUERY, HEADER -> true;
              default -> false;
            };
        if (location && binding.getMember().isRequired()) {
          return true;
        }
      }
    }
    return false;
  }

  /** Emits Parse<Op>Input(request, context) -> Outcome<Input> (inverse of the client request). */
  private void writeParseInputFunction(
      CppWriter w, CppContext context, SerdeCodeGen serde, OperationShape operation) {
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    String inputType = context.cppSymbols().toSymbol(input).getName();
    String opName = CppReservedWords.escape(operation.getId().getName());

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
                "cpp-codegen: restJson1 server binding "
                    + binding.getLocation()
                    + " is not supported yet ("
                    + operation.getId()
                    + ")");
      }
    }

    w.openBlock(
        "smithy::Outcome<$L> Parse$LInput(const smithy::http::HttpRequest& request, "
            + "const smithy::server::RequestContext& context, "
            + "std::vector<smithy::server::ValidationFailure>* validation_failures) {",
        inputType,
        opName);
    w.write("(void)request;");
    w.write("(void)context;");
    w.write("(void)validation_failures;");
    w.write("$L input{};", inputType);
    for (HttpBinding binding : labels.values()) {
      // @httpLabel members are always required, and route matching guarantees
      // the label was captured.
      MemberShape member = binding.getMember();
      w.openBlock("{");
      w.write("const std::string& label_value = context.labels.at($S);", binding.getLocationName());
      writeTextValueInto(
          w,
          context,
          serde,
          member,
          "label_value",
          "input." + context.cppSymbols().toMemberName(member),
          "smithy::TimestampFormat::kDateTime",
          /* push= */ false);
      w.closeBlock("}");
    }
    for (HttpBinding binding : headers.values()) {
      writeHeaderReadBinding(w, context, serde, binding, "request.headers", "input.");
      if (binding.getMember().isRequired()) {
        writeRequiredAbsenceFailure(
            w,
            binding.getMember(),
            "!request.headers.Get(\"" + binding.getLocationName() + "\").has_value()");
      }
    }
    if (!queries.isEmpty() || queryParams != null) {
      for (HttpBinding binding : queries.values()) {
        if (binding.getMember().isRequired()) {
          w.write("bool saw_$L = false;", context.cppSymbols().toMemberName(binding.getMember()));
        }
      }
      w.openBlock("for (const auto& [key, value] : context.query_params) {");
      for (HttpBinding binding : queries.values()) {
        MemberShape member = binding.getMember();
        Shape target = context.model().expectShape(member.getTarget());
        String field = "input." + context.cppSymbols().toMemberName(member);
        w.openBlock("if (key == $S) {", binding.getLocationName());
        if (member.isRequired()) {
          w.write("saw_$L = true;", context.cppSymbols().toMemberName(member));
        }
        if (target.isListShape()) {
          MemberShape element = target.asListShape().orElseThrow().getMember();
          if (!member.isRequired()) {
            w.write("if (!$L.has_value()) $L.emplace();", field, field);
          }
          String container = member.isRequired() ? field : "(*" + field + ")";
          writeTextValueInto(
              w,
              context,
              serde,
              element,
              "value",
              container + ".push_back",
              "smithy::TimestampFormat::kDateTime",
              /* push= */ true);
        } else {
          writeTextValueInto(
              w,
              context,
              serde,
              member,
              "value",
              field,
              "smithy::TimestampFormat::kDateTime",
              /* push= */ false);
        }
        w.write("continue;");
        w.closeBlock("}");
      }
      w.closeBlock("}");
      for (HttpBinding binding : queries.values()) {
        if (binding.getMember().isRequired()) {
          writeRequiredAbsenceFailure(
              w,
              binding.getMember(),
              "!saw_" + context.cppSymbols().toMemberName(binding.getMember()));
        }
      }
      if (queryParams != null) {
        MemberShape member = queryParams.getMember();
        var map = context.model().expectShape(member.getTarget()).asMapShape().orElseThrow();
        Shape valueTarget = context.model().expectShape(map.getValue().getTarget());
        String field = "input." + context.cppSymbols().toMemberName(member);
        w.write("// Servers put every query parameter in the @httpQueryParams map,");
        w.write("// including ones bound to other members.");
        w.openBlock("for (const auto& [key, value] : context.query_params) {");
        if (!member.isRequired()) {
          w.write("if (!$L.has_value()) $L.emplace();", field, field);
        }
        String container = member.isRequired() ? field : "(*" + field + ")";
        if (valueTarget.isListShape()) {
          w.write("$L[key].push_back(value);", container);
        } else {
          w.write("$L.emplace(key, value);", container);
        }
        w.closeBlock("}");
      }
    }
    if (!body.isEmpty()) {
      w.write(
          "auto body_doc = smithy::json::Decode(request.body.empty() ? \"{}\" : request.body);");
      w.write("if (!body_doc) return std::move(body_doc).error();");
      w.write(
          "if (!body_doc->is_map()) return smithy::Error::Serialization($S);",
          opName + ": expected a JSON object body");
      for (HttpBinding binding : body) {
        MemberShape member = binding.getMember();
        String wireName =
            member
                .getTrait(software.amazon.smithy.model.traits.JsonNameTrait.class)
                .map(software.amazon.smithy.model.traits.JsonNameTrait::getValue)
                .orElse(member.getMemberName());
        String field = "input." + context.cppSymbols().toMemberName(member);
        String path = inputType + "." + member.getMemberName();
        w.openBlock("{");
        w.write("const smithy::Document* member = body_doc->Find($S);", wireName);
        if (member.isRequired()) {
          w.openBlock("if (member == nullptr || member->is_null()) {");
          w.write(
              "AddValidationFailure(validation_failures, $S, $S);",
              "/" + member.getMemberName(),
              ValidationGenerator.memberMustNotBeNull("/" + member.getMemberName()));
          w.closeBlock("} else {");
          w.indent();
          serde.writeDeserializeInto(w, member, "member", field, path);
          w.closeBlock("}");
        } else {
          w.openBlock("if (member != nullptr && !member->is_null()) {");
          var targetType =
              context.cppSymbols().toSymbol(context.model().expectShape(member.getTarget()));
          w.write("$L parsed_member{};", targetType.getName());
          serde.writeDeserializeInto(w, member, "member", "parsed_member", path);
          w.write("$L = std::move(parsed_member);", field);
          w.closeBlock("}");
        }
        w.closeBlock("}");
      }
    }
    w.write("return input;");
    w.closeBlock("}");
    w.write("");
  }

  /** Records a "Member must not be null" failure when {@code absentCondition} holds. */
  private void writeRequiredAbsenceFailure(
      CppWriter w, MemberShape member, String absentCondition) {
    String path = "/" + member.getMemberName();
    w.write(
        "if ($L) AddValidationFailure(validation_failures, $S, $S);",
        absentCondition,
        path,
        ValidationGenerator.memberMustNotBeNull(path));
  }

  /**
   * Emits statements converting a decoded text {@code valueExpr} into {@code sink} (assignment
   * target, or a push_back callee when {@code push}). Runs inside an Outcome-returning function.
   */
  private void writeTextValueInto(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      MemberShape member,
      String valueExpr,
      String sink,
      String timestampDefault,
      boolean push) {
    Shape target = context.model().expectShape(member.getTarget());
    String open = push ? sink + "(" : sink + " = ";
    String close = push ? ");" : ";";
    switch (target.getType()) {
      case TIMESTAMP -> {
        w.write(
            "auto parsed_ts = smithy::Timestamp::Parse($L, $L);",
            valueExpr,
            serde.timestampFormat(member, timestampDefault));
        w.write("if (!parsed_ts) return std::move(parsed_ts).error();");
        w.write("$L*std::move(parsed_ts)$L", open, close);
      }
      case STRING -> w.write("$L$L$L", open, valueExpr, close);
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
              "cpp-codegen: label/query binding target " + target.getId() + " is not supported");
    }
  }

  /** Emits Serialize<Op>Response(output) (inverse of the client response handling). */
  private void writeSerializeResponseFunction(
      CppWriter w, CppContext context, SerdeCodeGen serde, OperationShape operation) {
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    HttpTrait http = operation.expectTrait(HttpTrait.class);
    StructureShape output = ProtocolSupport.outputShape(context, operation);
    String outputType = context.cppSymbols().toSymbol(output).getName();
    String opName = CppReservedWords.escape(operation.getId().getName());

    Map<String, HttpBinding> responseHeaders = new TreeMap<>();
    List<HttpBinding> responseBody = new java.util.ArrayList<>();
    HttpBinding responseCode = null;
    for (HttpBinding binding : index.getResponseBindings(operation).values()) {
      switch (binding.getLocation()) {
        case DOCUMENT -> responseBody.add(binding);
        case RESPONSE_CODE -> responseCode = binding;
        case HEADER -> responseHeaders.put(binding.getLocationName(), binding);
        default ->
            throw new CodegenException(
                "cpp-codegen: restJson1 server output binding "
                    + binding.getLocation()
                    + " is not supported yet ("
                    + operation.getId()
                    + ")");
      }
    }

    w.openBlock(
        "smithy::http::HttpResponse Serialize$LResponse(const $L& output) {", opName, outputType);
    w.write("(void)output;");
    w.write("smithy::http::HttpResponse response;");
    w.write("response.status = $L;", http.getCode());
    if (responseCode != null) {
      MemberShape codeMember = responseCode.getMember();
      String field = "output." + context.cppSymbols().toMemberName(codeMember);
      if (codeMember.isRequired()) {
        w.write("response.status = static_cast<int>($L);", field);
      } else {
        w.write("if ($L.has_value()) response.status = static_cast<int>(*$L);", field, field);
      }
    }
    for (HttpBinding binding : responseHeaders.values()) {
      writeHeaderWriteBinding(w, context, serde, binding, "output", "response.headers");
    }
    // restJson1 servers always produce a JSON body (at minimum "{}").
    w.write("smithy::DocumentMap body_map;");
    for (HttpBinding binding : responseBody) {
      MemberShape member = binding.getMember();
      String field = "output." + context.cppSymbols().toMemberName(member);
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
    w.write("response.headers.Set(\"content-type\", \"application/json\");");
    w.write("response.body = smithy::json::Encode(smithy::Document(std::move(body_map)));");
    w.write("return response;");
    w.closeBlock("}");
    w.write("");
  }

  @Override
  public void writeServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    HttpTrait http = operation.expectTrait(HttpTrait.class);
    String opName = CppReservedWords.escape(operation.getId().getName());
    StringBuilder pattern = new StringBuilder();
    for (SmithyPattern.Segment segment : http.getUri().getSegments()) {
      pattern.append('/');
      if (segment.isGreedyLabel()) {
        pattern.append('{').append(segment.getContent()).append("+}");
      } else if (segment.isLabel()) {
        pattern.append('{').append(segment.getContent()).append('}');
      } else {
        pattern.append(segment.getContent());
      }
    }
    if (pattern.length() == 0) {
      pattern.append('/');
    }
    boolean hasBodyBindings =
        HttpBindingIndex.of(context.model()).getRequestBindings(operation).values().stream()
            .anyMatch(b -> b.getLocation() == HttpBinding.Location.DOCUMENT);
    w.openBlock(
        "(void)router_->Add($S, $S, [handler](const smithy::http::HttpRequest& request, "
            + "const smithy::server::RequestContext& context) -> smithy::http::HttpResponse {",
        http.getMethod(),
        pattern.toString());
    if (hasBodyBindings) {
      w.write("// Content-Type validation per the HTTP binding spec: a present header");
      w.write("// must carry the protocol's media type (parameters ignored); 415 otherwise.");
      w.openBlock(
          "if (const auto content_type = request.headers.Get(\"content-type\"); "
              + "content_type.has_value() && "
              + "smithy::http::MediaTypeOf(*content_type) != \"application/json\") {");
      w.write(
          "return JsonError(415, \"UnsupportedMediaTypeException\", "
              + "\"expected content-type: application/json\", {});");
      w.closeBlock("}");
    }
    w.write("std::vector<smithy::server::ValidationFailure> validation_failures;");
    w.write("auto input = Parse$LInput(request, context, &validation_failures);", opName);
    if (emitsValidation) {
      w.write(
          "if (!validation_failures.empty()) "
              + "return ValidationErrorResponse(validation_failures);");
    }
    w.write("if (!input) return ErrorToResponse(input.error());");
    if (validation != null && validation.validates(operation)) {
      w.write("$L(*input, \"\", &validation_failures);", validation.validatorNameFor(operation));
      w.write(
          "if (!validation_failures.empty()) "
              + "return ValidationErrorResponse(validation_failures);");
    }
    w.write("auto outcome = handler->$L(*input);", opName);
    w.write("if (!outcome) return ErrorToResponse(outcome.error());");
    w.write("return Serialize$LResponse(*outcome);", opName);
    w.closeBlock("});");
  }
}
