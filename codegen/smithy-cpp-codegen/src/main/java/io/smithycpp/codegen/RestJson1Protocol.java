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
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.HttpTrait;

/** restJson1 client bindings: HTTP method/URI from @http, labels, query, headers, JSON bodies. */
final class RestJson1Protocol implements ProtocolGenerator {

  @Override
  public String name() {
    return "restJson1";
  }

  @Override
  public String contentType() {
    return "application/json";
  }

  @Override
  public List<String> runtimeDeps() {
    return List.of(":json");
  }

  @Override
  public List<String> clientIncludes() {
    return List.of("\"smithy/json/json.h\"");
  }

  @Override
  public void writeClientHelpers(CppWriter w, CppContext context) {
    ProtocolSupport.writeErrorDeserializer(
        w, "const auto doc = smithy::json::Decode(response.body);");
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
    for (HttpBinding binding : index.getRequestBindings(operation).values()) {
      switch (binding.getLocation()) {
        case LABEL -> labels.put(binding.getLocationName(), binding);
        case QUERY -> queries.put(binding.getLocationName(), binding);
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
    for (HttpBinding binding : index.getResponseBindings(operation).values()) {
      if (binding.getLocation() != HttpBinding.Location.DOCUMENT) {
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
    boolean hasQuery = !queries.isEmpty() || !http.getUri().getQueryLiterals().isEmpty();
    if (hasQuery) {
      w.write("smithy::http::QueryString query;");
      for (Map.Entry<String, String> literal :
          new TreeMap<>(http.getUri().getQueryLiterals()).entrySet()) {
        w.write("query.Add($S, $S);", literal.getKey(), literal.getValue());
      }
      for (HttpBinding binding : queries.values()) {
        writeSimpleBinding(
            w,
            context,
            serde,
            binding,
            in,
            "query.Add(\"" + binding.getLocationName() + "\", ",
            ");",
            "smithy::TimestampFormat::kDateTime");
      }
      w.write("target += query.ToString();");
    }

    w.write("smithy::http::HttpRequest request;");
    w.write("request.method = $S;", http.getMethod());
    w.write("request.target = std::move(target);");
    for (HttpBinding binding : headers.values()) {
      writeSimpleBinding(
          w,
          context,
          serde,
          binding,
          in,
          "request.headers.Set(\"" + binding.getLocationName() + "\", ",
          ");",
          "smithy::TimestampFormat::kHttpDate");
    }

    if (!body.isEmpty()) {
      w.write("smithy::DocumentMap body_map;");
      for (HttpBinding binding : body) {
        MemberShape member = binding.getMember();
        String field = in + "." + context.cppSymbols().toMemberName(member);
        if (member.isRequired()) {
          w.write(
              "body_map.emplace($S, $L);",
              member.getMemberName(),
              serde.serializeExpression(member, field));
        } else {
          w.openBlock("if ($L.has_value()) {", field);
          w.write(
              "body_map.emplace($S, $L);",
              member.getMemberName(),
              serde.serializeExpression(member, "*" + field));
          w.closeBlock("}");
        }
      }
      w.write("request.body = smithy::json::Encode(smithy::Document(std::move(body_map)));");
      w.write("request.headers.Set(\"content-type\", \"application/json\");");
    }

    w.write("auto response = Send(std::move(request));");
    w.write("if (!response) return std::move(response).error();");
    w.write("if (response->status != $L) return DeserializeError(*response);", http.getCode());

    String outType = context.cppSymbols().toSymbol(output).getName();
    if (output.members().isEmpty()) {
      w.write("return $L{};", outType);
      return;
    }
    boolean allOptional = output.members().stream().noneMatch(MemberShape::isRequired);
    if (allOptional) {
      w.write("if (response->body.empty()) return $L{};", outType);
    }
    w.write("auto body_doc = smithy::json::Decode(response->body);");
    w.write("if (!body_doc) return std::move(body_doc).error();");
    w.write("return Deserialize$L(*body_doc);", SerdeCodeGen.serdeFunctionSuffix(output));
  }

  private void writeSimpleBinding(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      HttpBinding binding,
      String in,
      String prefix,
      String suffix,
      String timestampDefault) {
    MemberShape member = binding.getMember();
    String field = in + "." + context.cppSymbols().toMemberName(member);
    if (member.isRequired()) {
      w.write(
          "$L$L$L",
          prefix,
          ProtocolSupport.toStringExpression(
              context, member, field, serde.timestampFormat(member, timestampDefault)),
          suffix);
    } else {
      w.openBlock("if ($L.has_value()) {", field);
      w.write(
          "$L$L$L",
          prefix,
          ProtocolSupport.toStringExpression(
              context, member, "*" + field, serde.timestampFormat(member, timestampDefault)),
          suffix);
      w.closeBlock("}");
    }
  }
}
