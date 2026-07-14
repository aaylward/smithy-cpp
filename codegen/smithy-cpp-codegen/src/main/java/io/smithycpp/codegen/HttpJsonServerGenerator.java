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

/**
 * The server half of the HTTP+JSON protocol: the JsonError/ErrorToResponse helpers, validation
 * wiring, one Parse&lt;Op&gt;Input / Serialize&lt;Op&gt;Response function pair per operation (each
 * the wire inverse of the client's request/response handling), and the per-operation route with its
 * content negotiation (415/406). Shared read/write emission lives in {@link HttpBindingCodeGen}.
 *
 * <p>Stateful: {@link #writeHelpers} builds the validation plan the routes consult, so it always
 * runs before {@link #writeRoute} (ServerGenerator emits helpers first).
 */
final class HttpJsonServerGenerator {

  /**
   * The response header carrying modeled-error identity (the error shape name), or "" when the
   * protocol carries no error header. simpleRestJson uses {@code x-error-type}.
   */
  private final String errorTypeHeaderName;

  /** Set up by writeHelpers (always called before the routes are emitted). */
  private ValidationGenerator validation;

  /** Whether the service emits ValidationErrorResponse (constraints or top-level @required). */
  private boolean emitsValidation;

  HttpJsonServerGenerator(String errorTypeHeaderName) {
    this.errorTypeHeaderName = errorTypeHeaderName;
  }

  List<String> includes() {
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

  /** Emits the error-type header set; a no-op when the protocol has no error header. */
  private void writeErrorTypeHeader(CppWriter w, String variable, String errorType) {
    if (!errorTypeHeaderName.isEmpty()) {
      w.write("$L.headers.Set($S, $S);", variable, errorTypeHeaderName, errorType);
    }
  }

  void writeHelpers(
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
        w, context, service, operations, "JsonError", errorTypeHeaderName);
    validation = new ValidationGenerator(context, operations);
    emitsValidation = validation.hasValidators() || anyTopLevelRequired(context, operations);
    if (emitsValidation) {
      ValidationGenerator.writeFailureHelper(w);
      validation.writeValidators(w);
      ValidationGenerator.writeValidationErrorResponse(w, "JsonError", "", errorTypeHeaderName);
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
    HttpBinding payload = null;
    HttpBinding prefixHeaders = null;
    for (HttpBinding binding : index.getRequestBindings(operation).values()) {
      switch (binding.getLocation()) {
        case LABEL -> labels.put(binding.getLocationName(), binding);
        case QUERY -> queries.put(binding.getLocationName(), binding);
        case QUERY_PARAMS -> queryParams = binding;
        case HEADER -> headers.put(binding.getLocationName(), binding);
        case DOCUMENT -> body.add(binding);
        case PAYLOAD -> payload = binding;
        case PREFIX_HEADERS -> prefixHeaders = binding;
        default ->
            throw new CodegenException(
                "cpp-codegen: HTTP+JSON server binding "
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
      HttpBindingCodeGen.writeHeaderReadBinding(
          w, context, serde, binding, "request.headers", "input.");
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
          boolean plainQuery = MemberDefaults.plain(context.model(), member);
          if (!plainQuery) {
            w.write("if (!$L.has_value()) $L.emplace();", field, field);
          }
          String container = plainQuery ? field : "(*" + field + ")";
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
        boolean plainMap = MemberDefaults.plain(context.model(), member);
        if (!plainMap) {
          w.write("if (!$L.has_value()) $L.emplace();", field, field);
        }
        String container = plainMap ? field : "(*" + field + ")";
        if (valueTarget.isListShape()) {
          w.write("$L[key].push_back(value);", container);
        } else {
          w.write("$L.emplace(key, value);", container);
        }
        w.closeBlock("}");
      }
    }
    if (prefixHeaders != null) {
      HttpBindingCodeGen.writePrefixHeadersRead(
          w, context, prefixHeaders, "request.headers", "input.");
    }
    if (payload != null) {
      HttpBindingCodeGen.writePayloadRead(w, context, serde, payload, "request.body", "input.");
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
        if (MemberDefaults.lenientRequired(context.model(), member)) {
          // @required + @default: absence keeps the default initializer.
          w.openBlock("if (member != nullptr && !member->is_null()) {");
          serde.writeDeserializeInto(w, member, "member", field, path);
          w.closeBlock("}");
        } else if (member.isRequired()) {
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
    // @default on @input members: clients skip them when unset, servers fill
    // the default for whatever the wire left unset (any binding location).
    for (MemberShape member : input.members()) {
      if (!MemberDefaults.fillOnParse(context.model(), member)) {
        continue;
      }
      String field = "input." + context.cppSymbols().toMemberName(member);
      w.write(
          "if (!$L.has_value()) $L = $L;", field, field, MemberDefaults.literal(context, member));
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
        String format = serde.timestampFormat(member, timestampDefault);
        if (format.equals("smithy::TimestampFormat::kDateTime")) {
          // Servers reject RFC3339 UTC offsets in text positions (clients must
          // accept them in responses, so Timestamp::Parse itself stays lenient).
          w.write(
              "if ($L.empty() || ($L.back() != 'Z' && $L.back() != 'z')) return "
                  + "smithy::Error::Serialization(\"expected a Z-terminated date-time\");",
              valueExpr,
              valueExpr,
              valueExpr);
        }
        w.write("auto parsed_ts = smithy::Timestamp::Parse($L, $L);", valueExpr, format);
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
      case BYTE, SHORT, INTEGER, LONG, INT_ENUM -> {
        w.write(
            "auto parsed_num = ParseInt64Text($L, $L);",
            valueExpr,
            ProtocolSupport.int64Bounds(target.getType()));
        w.write("if (!parsed_num) return std::move(parsed_num).error();");
        w.write(
            "$Lstatic_cast<$L>(*parsed_num)$L",
            open,
            context.cppSymbols().toSymbol(target).getName(),
            close);
      }
      case FLOAT -> {
        w.write("auto parsed_num = ParseDoubleText($L);", valueExpr);
        w.write("if (!parsed_num) return std::move(parsed_num).error();");
        w.write("$Lstatic_cast<float>(*parsed_num)$L", open, close);
      }
      case DOUBLE -> {
        w.write("auto parsed_num = ParseDoubleText($L);", valueExpr);
        w.write("if (!parsed_num) return std::move(parsed_num).error();");
        w.write("$L*parsed_num$L", open, close);
      }
      case BOOLEAN -> {
        w.write(
            "if ($L != \"true\" && $L != \"false\") return "
                + "smithy::Error::Serialization(\"expected a boolean, got: \" + $L);",
            valueExpr,
            valueExpr,
            valueExpr);
        w.write("$L($L == \"true\")$L", open, valueExpr, close);
      }
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
    HttpBinding responsePayload = null;
    HttpBinding responsePrefixHeaders = null;
    for (HttpBinding binding : index.getResponseBindings(operation).values()) {
      switch (binding.getLocation()) {
        case DOCUMENT -> responseBody.add(binding);
        case RESPONSE_CODE -> responseCode = binding;
        case PAYLOAD -> responsePayload = binding;
        case PREFIX_HEADERS -> responsePrefixHeaders = binding;
        case HEADER -> responseHeaders.put(binding.getLocationName(), binding);
        default ->
            throw new CodegenException(
                "cpp-codegen: HTTP+JSON server output binding "
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
      if (MemberDefaults.plain(context.model(), codeMember)) {
        w.write("response.status = static_cast<int>($L);", field);
      } else {
        w.write("if ($L.has_value()) response.status = static_cast<int>(*$L);", field, field);
      }
    }
    for (HttpBinding binding : responseHeaders.values()) {
      HttpBindingCodeGen.writeHeaderWriteBinding(
          w, context, serde, binding, "output", "response.headers");
    }
    if (responsePrefixHeaders != null) {
      HttpBindingCodeGen.writePrefixHeadersWrite(
          w, context, responsePrefixHeaders, "output", "response");
    }
    if (responsePayload != null) {
      HttpBindingCodeGen.writePayloadWrite(
          w, context, serde, operation, responsePayload, "output", "response", false);
      w.write("return response;");
      w.closeBlock("}");
      w.write("");
      return;
    }
    if (http.getCode() == 204) {
      // 204 No Content responses must not carry a body (or content-type).
      w.write("return response;");
      w.closeBlock("}");
      w.write("");
      return;
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
      if (MemberDefaults.plain(context.model(), member)) {
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

  void writeRoute(CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
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
    HttpBindingIndex bindingIndex = HttpBindingIndex.of(context.model());
    boolean hasResponseContent =
        bindingIndex.getResponseBindings(operation).values().stream()
            .anyMatch(
                b ->
                    b.getLocation() == HttpBinding.Location.DOCUMENT
                        || b.getLocation() == HttpBinding.Location.PAYLOAD);
    boolean compressed =
        operation
            .getTrait(software.amazon.smithy.model.traits.RequestCompressionTrait.class)
            .map(t -> t.getEncodings().contains("gzip"))
            .orElse(false);
    w.openBlock(
        "(void)router_->Add($S, $S, [handler](const smithy::http::HttpRequest& $L, "
            + "const smithy::server::RequestContext& context) -> smithy::http::HttpResponse {",
        http.getMethod(),
        pattern.toString(),
        compressed ? "raw_request" : "request");
    if (compressed) {
      w.write("smithy::http::HttpRequest request = raw_request;");
      ProtocolSupport.writeRequestDecompression(w, operation, "JsonError", "");
    }
    StructureShape inputShape = ProtocolSupport.inputShape(context, operation);
    boolean noModeledInput =
        inputShape.getId().toString().equals("smithy.api#Unit")
            || inputShape
                .getTrait(software.amazon.smithy.model.traits.synthetic.OriginalShapeIdTrait.class)
                .map(t -> t.getOriginalId().toString().equals("smithy.api#Unit"))
                .orElse(false);
    w.write("// Content-Type validation per the HTTP binding spec (415), then Accept (406);");
    w.write("// the malformed-request suite pins the error-identity headers. A missing");
    w.write("// content-type is tolerated, and blob payloads without @mediaType accept");
    w.write("// any content type / accept.");
    if (noModeledInput) {
      w.openBlock("if (request.headers.Get(\"content-type\").has_value()) {");
      w.write("auto error_response = JsonError(415, \"\", \"unsupported media type\", {});");
      writeErrorTypeHeader(w, "error_response", "UnsupportedMediaTypeException");
      w.write("return error_response;");
      w.closeBlock("}");
    } else if (!HttpBindingCodeGen.lenientPayload(
        context, bindingIndex, operation, /* response= */ false)) {
      String requestContentType =
          HttpBindingCodeGen.payloadContentType(context, operation, /* isRequest= */ true);
      String expected = requestContentType.isEmpty() ? "application/json" : requestContentType;
      boolean hasRequestPayload =
          bindingIndex.getRequestBindings(operation).values().stream()
              .anyMatch(b -> b.getLocation() == HttpBinding.Location.PAYLOAD);
      // Payload-bound requests additionally require a content-type once a body
      // is present; document bodies tolerate a missing header (the server-mode
      // httpRequestTests pin that leniency).
      String condition =
          hasRequestPayload
              ? "content_type.has_value() ? smithy::http::MediaTypeOf(*content_type) != $S "
                  + ": !request.body.empty()"
              : "content_type.has_value() && smithy::http::MediaTypeOf(*content_type) != $S";
      w.openBlock(
          "if (const auto content_type = request.headers.Get(\"content-type\"); "
              + condition
              + ") {",
          expected);
      w.write("auto error_response = JsonError(415, \"\", \"unsupported media type\", {});");
      writeErrorTypeHeader(w, "error_response", "UnsupportedMediaTypeException");
      w.write("return error_response;");
      w.closeBlock("}");
    }
    if (hasResponseContent
        && !HttpBindingCodeGen.lenientPayload(
            context, bindingIndex, operation, /* response= */ true)) {
      String determined =
          HttpBindingCodeGen.payloadContentType(context, operation, /* isRequest= */ false);
      String responseContentType = determined.isEmpty() ? "application/json" : determined;
      w.openBlock(
          "if (const auto accept = request.headers.Get(\"accept\"); accept.has_value() && "
              + "!smithy::http::AcceptMatches(*accept, $S)) {",
          responseContentType);
      w.write("auto error_response = JsonError(406, \"\", \"not acceptable\", {});");
      writeErrorTypeHeader(w, "error_response", "NotAcceptableException");
      w.write("return error_response;");
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
    w.closeBlock("}, $S);", operation.getId().getName());
  }
}
