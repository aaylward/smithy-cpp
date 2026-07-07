package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;

/**
 * smithy.cpp.protocols#jsonRpc2: a vendor-neutral JSON-RPC 2.0 protocol. Every operation POSTs to a
 * single endpoint with body {@code {"jsonrpc":"2.0","method":<Operation>,"params":<input>,"id":1}};
 * success responds {@code {"jsonrpc":"2.0","result":<output>,"id":1}} and a modeled error responds
 * (still HTTP 200) {@code
 * {"jsonrpc":"2.0","error":{"code":-32000,"message":..,"data":{"__type":..}}, "id":1}}. JSON bodies
 * over the shared {@code smithy::Document} serde pivot.
 */
final class JsonRpc2Protocol implements ProtocolGenerator {

  static final ShapeId TRAIT = ShapeId.from("smithy.cpp.protocols#jsonRpc2");

  /** JSON-RPC server-error code carrying a modeled Smithy error. */
  private static final int MODELED_ERROR_CODE = -32000;

  @Override
  public String name() {
    return "jsonRpc2";
  }

  @Override
  public ShapeId traitId() {
    return TRAIT;
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
  public List<String> runtimeDeps() {
    return List.of(":json");
  }

  @Override
  public List<String> clientIncludes() {
    return List.of("\"smithy/json/json.h\"", "\"smithy/core/base64.h\"", "<cstdint>");
  }

  @Override
  public void writeClientHelpers(CppWriter w, CppContext context) {
    // Error scaffolding compatible with the shared per-operation error
    // deserializers (ParsedError / GenericError / ParseError signatures), but
    // reading the JSON-RPC error object: code and message live under "error",
    // and the modeled-error identity + members live under "error"."data".
    w.write("std::string SanitizeErrorCode(std::string_view raw) {");
    w.write(
        "if (const auto hash = raw.find('#'); hash != std::string_view::npos) "
            + "raw = raw.substr(hash + 1);");
    w.write("return std::string(raw);");
    w.write("}");
    w.write("");
    w.openBlock("struct ParsedError {");
    w.write("int status = 0;");
    w.write("std::string code = \"UnknownError\";");
    w.write("std::string message;");
    w.write("smithy::Document doc;");
    w.closeBlock("};");
    w.write("");
    w.openBlock("ParsedError ParseError(const smithy::http::HttpResponse& response) {");
    w.write("ParsedError parsed;");
    w.write("parsed.message = \"jsonRpc2 error\";");
    w.write("auto decoded = smithy::json::Decode(response.body);");
    w.write("if (!decoded || !decoded->is_map()) return parsed;");
    w.write("const smithy::Document* error = decoded->Find(\"error\");");
    w.write("if (error == nullptr || !error->is_map()) return parsed;");
    w.write("const smithy::Document* message = error->Find(\"message\");");
    w.write(
        "if (message != nullptr && message->is_string()) parsed.message = message->as_string();");
    w.write("const smithy::Document* code = error->Find(\"code\");");
    w.write(
        "if (code != nullptr && code->is_int()) parsed.status = "
            + "code->as_int() <= -32000 ? 500 : 400;");
    w.write("const smithy::Document* data = error->Find(\"data\");");
    w.openBlock("if (data != nullptr && data->is_map()) {");
    w.write("parsed.doc = *data;");
    w.write("const smithy::Document* type = data->Find(\"__type\");");
    w.write(
        "if (type != nullptr && type->is_string()) parsed.code = "
            + "SanitizeErrorCode(type->as_string());");
    w.closeBlock("}");
    w.write("return parsed;");
    w.closeBlock("}");
    w.write("");
    w.openBlock("smithy::Error GenericError(ParsedError parsed) {");
    w.write("const bool retryable = parsed.status >= 500;");
    w.write(
        "if (parsed.code == \"UnknownError\") return smithy::Error(smithy::ErrorKind::kUnknown, "
            + "std::move(parsed.code), std::move(parsed.message), retryable);");
    w.write(
        "return smithy::Error::Modeled(std::move(parsed.code), std::move(parsed.message), "
            + "retryable);");
    w.closeBlock("}");
    w.write("");
    ProtocolSupport.writeNumericParseHelpers(w);
  }

  @Override
  public void writeOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StructureShape output = ProtocolSupport.outputShape(context, operation);
    String in =
        ProtocolSupport.prepareIdempotencyTokens(
            w, context, input, context.cppSymbols().toSymbol(input).getName());

    w.write("smithy::http::HttpRequest request;");
    w.write("request.method = \"POST\";");
    w.write("request.target = path_prefix_.empty() ? \"/\" : path_prefix_;");
    w.write("request.headers.Set(\"content-type\", \"application/json\");");
    w.write("smithy::DocumentMap envelope;");
    w.write("envelope.insert_or_assign(\"jsonrpc\", smithy::Document(\"2.0\"));");
    w.write(
        "envelope.insert_or_assign(\"method\", smithy::Document($S));", jsonRpcMethod(operation));
    if (!isUnit(input)) {
      w.write(
          "envelope.insert_or_assign(\"params\", Serialize$L($L));",
          SerdeCodeGen.serdeFunctionSuffix(context, input),
          in);
    }
    // HTTP correlates request and response, so a constant id is sufficient.
    w.write("envelope.insert_or_assign(\"id\", smithy::Document(std::int64_t{1}));");
    w.write("request.body = smithy::json::Encode(smithy::Document(std::move(envelope)));");
    w.write("auto response = Send(std::move(request));");
    w.write("if (!response) return std::move(response).error();");
    w.write("auto body = smithy::json::Decode(response->body);");
    w.write(
        "if (!body || !body->is_map()) return smithy::Error::Serialization("
            + "\"jsonRpc2: malformed response\");");
    // JSON-RPC signals failure with an "error" member (HTTP status is 200).
    w.write(
        "if (body->Find(\"error\") != nullptr) return $L;",
        ProtocolSupport.errorExpression(context, service, operation));

    String outType = context.cppSymbols().toSymbol(output).getName();
    if (output.members().isEmpty()) {
      w.write("return $L{};", outType);
      return;
    }
    w.write("const smithy::Document* result = body->Find(\"result\");");
    boolean allOptional = output.members().stream().noneMatch(MemberShape::isRequired);
    if (allOptional) {
      w.write("if (result == nullptr) return $L{};", outType);
    } else {
      w.write(
          "if (result == nullptr) return smithy::Error::Serialization("
              + "\"jsonRpc2: response missing result\");");
    }
    w.write("return Deserialize$L(*result);", SerdeCodeGen.serdeFunctionSuffix(context, output));
  }

  /** Set up by writeServerHelpers (always called before the routes are emitted). */
  private ValidationGenerator validation;

  @Override
  public List<String> serverIncludes() {
    return List.of("\"smithy/json/json.h\"", "\"smithy/core/base64.h\"");
  }

  @Override
  public void writeServerHelpers(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    // {"jsonrpc":"2.0","result":<doc>,"id":<id>}
    w.openBlock(
        "smithy::http::HttpResponse RpcResult(const smithy::Document& id, smithy::Document result) {");
    w.write("smithy::DocumentMap body;");
    w.write("body.insert_or_assign(\"jsonrpc\", smithy::Document(\"2.0\"));");
    w.write("body.insert_or_assign(\"result\", std::move(result));");
    w.write("body.insert_or_assign(\"id\", id);");
    w.write("smithy::http::HttpResponse response;");
    w.write("response.headers.Set(\"content-type\", \"application/json\");");
    w.write("response.body = smithy::json::Encode(smithy::Document(std::move(body)));");
    w.write("return response;");
    w.closeBlock("}");
    w.write("");
    // {"jsonrpc":"2.0","error":{"code":..,"message":..,"data":..},"id":<id>}
    w.openBlock(
        "smithy::http::HttpResponse RpcError(smithy::Document id, int code, "
            + "const std::string& message, smithy::DocumentMap data) {");
    w.write("smithy::DocumentMap error;");
    w.write("error.insert_or_assign(\"code\", smithy::Document(code));");
    w.write("error.insert_or_assign(\"message\", smithy::Document(message));");
    w.write(
        "if (!data.empty()) error.insert_or_assign(\"data\", smithy::Document(std::move(data)));");
    w.write("smithy::DocumentMap body;");
    w.write("body.insert_or_assign(\"jsonrpc\", smithy::Document(\"2.0\"));");
    w.write("body.insert_or_assign(\"error\", smithy::Document(std::move(error)));");
    w.write("body.insert_or_assign(\"id\", std::move(id));");
    w.write("smithy::http::HttpResponse response;");
    w.write("response.headers.Set(\"content-type\", \"application/json\");");
    w.write("response.body = smithy::json::Encode(smithy::Document(std::move(body)));");
    w.write("return response;");
    w.closeBlock("}");
    w.write("");
    writeErrorToResponse(w, context, service, operations);
    validation = new ValidationGenerator(context, operations);
    if (validation.hasValidators()) {
      ValidationGenerator.writeFailureHelper(w);
      validation.writeValidators(w);
      writeValidationErrorResponse(w);
    }
  }

  /** Maps a modeled smithy::Error to a JSON-RPC error object (data carries __type + members). */
  private void writeErrorToResponse(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    java.util.Map<String, StructureShape> errorShapes = new java.util.TreeMap<>();
    for (OperationShape operation : operations) {
      for (ShapeId errorId : operation.getErrors(service)) {
        StructureShape shape =
            context.model().expectShape(errorId).asStructureShape().orElseThrow();
        errorShapes.put(context.cppSymbols().toSymbol(shape).getName(), shape);
      }
    }
    w.openBlock(
        "smithy::http::HttpResponse ErrorToResponse(smithy::Document id, "
            + "const smithy::Error& error) {");
    w.openBlock("if (error.kind() == smithy::ErrorKind::kModeled) {");
    for (StructureShape shape : errorShapes.values()) {
      String type = context.cppSymbols().toSymbol(shape).getName();
      w.openBlock("if (error.code() == $S) {", shape.getId().getName());
      w.write("smithy::DocumentMap data;");
      w.openBlock("if (const auto* detail = error.detail<$L>()) {", type);
      w.write(
          "data = Serialize$L(*detail).as_map();",
          SerdeCodeGen.serdeFunctionSuffix(context, shape));
      w.closeBlock("}");
      w.write("data.insert_or_assign(\"__type\", smithy::Document($S));", shape.getId().getName());
      w.write(
          "return RpcError(std::move(id), $L, error.message(), std::move(data));",
          MODELED_ERROR_CODE);
      w.closeBlock("}");
    }
    w.closeBlock("}");
    // Validation/serialization failures -> invalid params; anything else -> internal error.
    w.openBlock(
        "if (error.kind() == smithy::ErrorKind::kValidation || error.kind() == "
            + "smithy::ErrorKind::kSerialization) {");
    w.write("return RpcError(std::move(id), -32602, error.message(), {});");
    w.closeBlock("}");
    w.write("return RpcError(std::move(id), -32603, error.message(), {});");
    w.closeBlock("}");
    w.write("");
  }

  private void writeValidationErrorResponse(CppWriter w) {
    w.addInclude("<cstddef>");
    w.openBlock(
        "smithy::http::HttpResponse ValidationErrorResponse(smithy::Document id, "
            + "const std::vector<smithy::server::ValidationFailure>& failures) {");
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
    w.write("smithy::DocumentMap data;");
    w.write("data.emplace(\"__type\", smithy::Document(\"ValidationException\"));");
    w.write("data.emplace(\"fieldList\", smithy::Document(std::move(field_list)));");
    w.write("return RpcError(std::move(id), -32602, summary, std::move(data));");
    w.closeBlock("}");
    w.write("");
  }

  @Override
  public void writeServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    throw new UnsupportedOperationException("jsonRpc2 registers a single combined route");
  }

  @Override
  public void writeServerRoutes(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    w.openBlock(
        "(void)router_->Add(\"POST\", \"/\", [handler](const smithy::http::HttpRequest& request, "
            + "const smithy::server::RequestContext&) -> smithy::http::HttpResponse {");
    w.write("auto decoded = smithy::json::Decode(request.body);");
    w.write("smithy::Document id;");
    w.openBlock("if (!decoded || !decoded->is_map()) {");
    w.write("return RpcError(id, -32700, \"parse error\", {});");
    w.closeBlock("}");
    w.write("if (const smithy::Document* rid = decoded->Find(\"id\"); rid != nullptr) id = *rid;");
    w.write("const smithy::Document* method = decoded->Find(\"method\");");
    w.openBlock("if (method == nullptr || !method->is_string()) {");
    w.write("return RpcError(id, -32600, \"invalid request: missing method\", {});");
    w.closeBlock("}");
    w.write("const smithy::Document* params_ptr = decoded->Find(\"params\");");
    w.write(
        "smithy::Document params = params_ptr != nullptr ? *params_ptr "
            + ": smithy::Document(smithy::DocumentMap{});");
    w.write("const std::string& method_name = method->as_string();");
    for (OperationShape operation : operations) {
      writeMethodBranch(w, context, operation);
    }
    w.write("return RpcError(id, -32601, \"method not found: \" + method_name, {});");
    w.closeBlock("});");
  }

  private void writeMethodBranch(CppWriter w, CppContext context, OperationShape operation) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StructureShape output = ProtocolSupport.outputShape(context, operation);
    String inputType = context.cppSymbols().toSymbol(input).getName();
    String opName = CppReservedWords.escape(operation.getId().getName());

    w.openBlock("if (method_name == $S) {", jsonRpcMethod(operation));
    w.write("$L input{};", inputType);
    if (!isUnit(input)) {
      w.write(
          "auto parsed = Deserialize$L(params);", SerdeCodeGen.serdeFunctionSuffix(context, input));
      w.write("if (!parsed) return RpcError(id, -32602, parsed.error().message(), {});");
      w.write("input = *std::move(parsed);");
    }
    if (validation != null && validation.validates(operation)) {
      w.write("std::vector<smithy::server::ValidationFailure> validation_failures;");
      w.write("$L(input, \"\", &validation_failures);", validation.validatorNameFor(operation));
      w.write(
          "if (!validation_failures.empty()) "
              + "return ValidationErrorResponse(id, validation_failures);");
    }
    w.write("auto outcome = handler->$L(input);", opName);
    w.write("if (!outcome) return ErrorToResponse(id, outcome.error());");
    if (output.members().isEmpty()) {
      w.write("return RpcResult(id, smithy::Document(smithy::DocumentMap{}));");
    } else {
      w.write(
          "return RpcResult(id, Serialize$L(*outcome));",
          SerdeCodeGen.serdeFunctionSuffix(context, output));
    }
    w.closeBlock("}");
  }

  private static String jsonRpcMethod(OperationShape operation) {
    return operation.getId().getName();
  }

  private static boolean isUnit(StructureShape shape) {
    if (shape.getId().toString().equals("smithy.api#Unit")) {
      return true;
    }
    return shape
        .getTrait(software.amazon.smithy.model.traits.synthetic.OriginalShapeIdTrait.class)
        .map(t -> t.getOriginalId().toString().equals("smithy.api#Unit"))
        .orElse(false);
  }
}
