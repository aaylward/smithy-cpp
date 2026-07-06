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
        /* errorTypeHeader= */ false);
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
    boolean noModeledInput =
        input.getId().toString().equals("smithy.api#Unit")
            || input
                .getTrait(software.amazon.smithy.model.traits.synthetic.OriginalShapeIdTrait.class)
                .map(t -> t.getOriginalId().toString().equals("smithy.api#Unit"))
                .orElse(false);
    if (!noModeledInput) {
      w.write("request.headers.Set(\"content-type\", \"application/cbor\");");
      w.write(
          "request.body = smithy::cbor::Encode(Serialize$L($L)).ToString();",
          SerdeCodeGen.serdeFunctionSuffix(context, input),
          in);
    }

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
