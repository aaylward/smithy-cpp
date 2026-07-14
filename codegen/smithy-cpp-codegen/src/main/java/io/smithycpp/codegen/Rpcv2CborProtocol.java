package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.StructureShape;

/**
 * smithy.protocols#rpcv2Cbor client bindings: POST /service/{Service}/operation/{Operation}, the
 * smithy-protocol header, CBOR request/response/error bodies.
 */
final class Rpcv2CborProtocol implements ProtocolGenerator {

  @Override
  public String name() {
    return "rpcv2Cbor";
  }

  @Override
  public software.amazon.smithy.model.shapes.ShapeId traitId() {
    return software.amazon.smithy.protocol.traits.Rpcv2CborTrait.ID;
  }

  @Override
  public String contentType() {
    return "application/cbor";
  }

  @Override
  public List<String> runtimeDeps() {
    return List.of(":cbor");
  }

  @Override
  public List<String> clientIncludes() {
    return List.of("\"smithy/cbor/cbor.h\"", "\"smithy/core/blob.h\"");
  }

  @Override
  public void writeClientHelpers(CppWriter w, CppContext context) {
    ProtocolSupport.writeErrorSupport(
        w,
        "auto doc = smithy::cbor::Decode(smithy::Blob::FromString(response.body));",
        /* errorTypeHeader= */ "");
  }

  /** Set up by writeServerHelpers (always called before the routes are emitted). */
  private ValidationGenerator validation;

  @Override
  public List<String> serverIncludes() {
    return List.of("\"smithy/cbor/cbor.h\"", "\"smithy/core/blob.h\"");
  }

  @Override
  public void writeServerHelpers(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    ProtocolSupport.writeErrorBodyHelper(
        w,
        "CborError",
        "application/cbor",
        "smithy::cbor::Encode(smithy::Document(std::move(body))).ToString()",
        "smithy-protocol",
        "rpc-v2-cbor");
    ProtocolSupport.writeServerErrorToResponse(
        w, context, service, operations, "CborError", /* errortypeHeader= */ "");
    // rpcv2Cbor error identity travels in the body, as the fully qualified shape id.
    validation =
        ValidationGenerator.writeWiring(
            w,
            context,
            operations,
            /* alsoEmit= */ false,
            "CborError",
            "smithy.framework#ValidationException",
            /* errortypeHeader= */ "",
            "",
            "");
  }

  @Override
  public void writeServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StructureShape output = ProtocolSupport.outputShape(context, operation);
    String inputType = context.cppSymbols().toSymbol(input).getName();
    String opName = CppReservedWords.escape(operation.getId().getName());

    boolean compressed = ProtocolSupport.gzipCompressed(operation);
    w.openBlock(
        "(void)router_->Add(\"POST\", \"/service/$L/operation/$L\", "
            + "[handler](const smithy::http::HttpRequest& $L, "
            + "const smithy::server::RequestContext&) -> smithy::http::HttpResponse {",
        service.getId().getName(),
        operation.getId().getName(),
        compressed ? "raw_request" : "request");
    if (compressed) {
      w.write("smithy::http::HttpRequest request = raw_request;");
      ProtocolSupport.writeRequestDecompression(
          w, operation, "CborError", "SerializationException");
    }
    w.openBlock(
        "if (request.headers.Get(\"smithy-protocol\").value_or(\"\") != \"rpc-v2-cbor\") {");
    w.write(
        "return CborError(400, \"SerializationException\", "
            + "\"expected smithy-protocol: rpc-v2-cbor\", {});");
    w.closeBlock("}");
    w.write("// Content-Type validation per the rpcv2Cbor spec: a present header must");
    w.write("// carry application/cbor (parameters ignored); 415 otherwise.");
    w.openBlock(
        "if (const auto content_type = request.headers.Get(\"content-type\"); "
            + "content_type.has_value() && "
            + "smithy::http::MediaTypeOf(*content_type) != \"application/cbor\") {");
    w.write(
        "return CborError(415, \"UnsupportedMediaTypeException\", "
            + "\"expected content-type: application/cbor\", {});");
    w.closeBlock("}");
    w.write("$L input{};", inputType);
    if (!ProtocolSupport.noModeledInput(input)) {
      w.write("// An absent body deserializes like an empty CBOR map.");
      w.write("smithy::Document body_doc{smithy::DocumentMap{}};");
      w.openBlock("if (!request.body.empty()) {");
      w.write("auto decoded = smithy::cbor::Decode(smithy::Blob::FromString(request.body));");
      w.write(
          "if (!decoded) return CborError(400, \"SerializationException\", "
              + "decoded.error().message(), {});");
      w.write("body_doc = *std::move(decoded);");
      w.closeBlock("}");
      w.write(
          "auto parsed = Deserialize$L(body_doc);",
          SerdeCodeGen.serdeFunctionSuffix(context, input));
      w.write(
          "if (!parsed) return CborError(400, \"SerializationException\", "
              + "parsed.error().message(), {});");
      w.write("input = *std::move(parsed);");
    }
    validation.writeRouteGuard(w, operation, "");
    w.write("auto outcome = handler->$L(input);", opName);
    w.write("if (!outcome) return ErrorToResponse(outcome.error());");
    w.write("smithy::http::HttpResponse response;");
    w.write("response.headers.Set(\"smithy-protocol\", \"rpc-v2-cbor\");");
    w.write("response.headers.Set(\"content-type\", \"application/cbor\");");
    w.write(
        "response.body = smithy::cbor::Encode(Serialize$L(*outcome)).ToString();",
        SerdeCodeGen.serdeFunctionSuffix(context, output));
    w.write("return response;");
    w.closeBlock("}, $S);", operation.getId().getName());
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
    w.write(
        "request.target = path_prefix_ + \"/service/$L/operation/$L\";",
        service.getId().getName(),
        operation.getId().getName());
    w.write("request.headers.Set(\"smithy-protocol\", \"rpc-v2-cbor\");");
    // Operations with no modeled input (smithy.api#Unit) send no body and no
    // Content-Type, per the rpcv2Cbor spec; empty input structures still send
    // an (empty) CBOR map body.
    if (!ProtocolSupport.noModeledInput(input)) {
      w.write("request.headers.Set(\"content-type\", \"application/cbor\");");
      w.write(
          "request.body = smithy::cbor::Encode(Serialize$L($L)).ToString();",
          SerdeCodeGen.serdeFunctionSuffix(context, input),
          in);
    }

    ProtocolSupport.writeRequestCompression(w, operation);
    w.write("auto response = Send(std::move(request));");
    w.write("if (!response) return std::move(response).error();");
    w.write(
        "if (response->status != 200) return $L;",
        ProtocolSupport.errorExpression(context, service, operation));

    String outType = context.cppSymbols().toSymbol(output).getName();
    if (output.members().isEmpty()) {
      w.write("return $L{};", outType);
      return;
    }
    boolean allOptional = output.members().stream().noneMatch(MemberShape::isRequired);
    if (allOptional) {
      w.write("if (response->body.empty()) return $L{};", outType);
    }
    w.write("auto body_doc = smithy::cbor::Decode(smithy::Blob::FromString(response->body));");
    w.write("if (!body_doc) return std::move(body_doc).error();");
    w.write("return Deserialize$L(*body_doc);", SerdeCodeGen.serdeFunctionSuffix(context, output));
  }
}
