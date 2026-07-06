package io.smithycpp.codegen;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import software.amazon.smithy.model.knowledge.TopDownIndex;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.DocumentationTrait;

/** Emits client.h / src/client.cc: the <Service>Client class over the protocol generator. */
final class ClientGenerator {

  private final CppContext context;
  private final ServiceShape service;
  private final ProtocolGenerator protocol;

  ClientGenerator(CppContext context, ServiceShape service, ProtocolGenerator protocol) {
    this.context = context;
    this.service = service;
    this.protocol = protocol;
  }

  List<OperationShape> operations() {
    List<OperationShape> operations =
        new ArrayList<>(TopDownIndex.of(context.model()).getContainedOperations(service));
    operations.sort(Comparator.comparing(OperationShape::getId));
    return operations;
  }

  private String clientName() {
    return CppReservedWords.escape(service.getId().getName()) + "Client";
  }

  void run() {
    context
        .writerDelegator()
        .useFileWriter(context.settings().clientHeaderFile(), this::writeHeader);
    context.writerDelegator().useFileWriter("src/client.cc", this::writeSource);
  }

  private void writeHeader(CppWriter w) {
    w.addInclude("<memory>");
    w.addInclude("<string>");
    w.addInclude("\"" + context.settings().includePrefix() + "/types.h\"");
    w.addInclude("\"smithy/client/config.h\"");
    w.addInclude("\"smithy/core/outcome.h\"");
    w.addInclude("\"smithy/http/transport.h\"");

    String name = clientName();
    w.write("/// $L client for $L.", protocol.name(), service.getId().toString());
    w.write("/// Modeled service errors surface as smithy::Error with kind kModeled,");
    w.write("/// code() set to the error shape name, and the deserialized error");
    w.write("/// structure attached: error.detail<TheErrorShape>().");
    w.openBlock("class $L {", name);
    w.write("public:").indent();
    w.write("/// Fails when the endpoint cannot be parsed and no transport is injected.");
    w.write("static smithy::Outcome<$L> Create(smithy::ClientConfig config);", name);
    w.write("");
    for (OperationShape operation : operations()) {
      operation
          .getTrait(DocumentationTrait.class)
          .ifPresent(
              docs -> {
                for (String line : docs.getValue().split("\n", -1)) {
                  w.write("/// $L", line);
                }
              });
      StructureShape input = ProtocolSupport.inputShape(context, operation);
      String inputType = context.cppSymbols().toSymbol(input).getName();
      String outputType =
          context.cppSymbols().toSymbol(ProtocolSupport.outputShape(context, operation)).getName();
      String defaulted = input.members().isEmpty() ? " = {}" : "";
      w.write(
          "smithy::Outcome<$L> $L(const $L& input$L) const;",
          outputType,
          CppReservedWords.escape(operation.getId().getName()),
          inputType,
          defaulted);
    }
    w.write("").dedent();
    w.write("private:").indent();
    w.write(
        "$L(smithy::ClientConfig config, std::shared_ptr<smithy::http::HttpClient> "
            + "transport, std::string path_prefix);",
        name);
    w.write(
        "smithy::Outcome<smithy::http::HttpResponse> "
            + "Send(smithy::http::HttpRequest request) const;");
    w.write("");
    w.write("smithy::ClientConfig config_;");
    w.write("std::shared_ptr<smithy::http::HttpClient> transport_;");
    w.write("std::string path_prefix_;").dedent();
    w.closeBlock("};");
    w.write("");
  }

  private void writeSource(CppWriter w) {
    for (String include : ProtocolSupport.sharedClientIncludes(context)) {
      w.addInclude(include);
    }
    for (String include : protocol.clientIncludes()) {
      w.addInclude(include);
    }
    String name = clientName();

    w.write("namespace {");
    w.write("");
    protocol.writeClientHelpers(w, context);
    ProtocolSupport.writeOperationErrorDeserializers(w, context, service, protocol, operations());
    w.write("}  // namespace");
    w.write("");

    w.openBlock("smithy::Outcome<$L> $L::Create(smithy::ClientConfig config) {", name, name);
    w.write("std::shared_ptr<smithy::http::HttpClient> transport = config.http_client;");
    w.write("std::string prefix;");
    w.openBlock("if (!config.endpoint.empty()) {");
    w.write("auto endpoint = smithy::http::ParseEndpoint(config.endpoint);");
    w.write("if (!endpoint) return std::move(endpoint).error();");
    w.write("prefix = endpoint->path_prefix;");
    w.openBlock("if (transport == nullptr) {");
    w.write(
        "transport = std::make_shared<smithy::http::SocketHttpClient>(endpoint->host, "
            + "endpoint->port, config.request_timeout_ms);");
    w.closeBlock("}");
    w.closeBlock("}");
    w.openBlock("if (transport == nullptr) {");
    w.write(
        "return smithy::Error::Validation($S);",
        name + ": config needs an endpoint or an http_client");
    w.closeBlock("}");
    w.write("return $L(std::move(config), std::move(transport), std::move(prefix));", name);
    w.closeBlock("}");
    w.write("");

    w.openBlock(
        "$L::$L(smithy::ClientConfig config, "
            + "std::shared_ptr<smithy::http::HttpClient> transport, std::string path_prefix)",
        name,
        name);
    w.write(": config_(std::move(config)),");
    w.write("  transport_(std::move(transport)),");
    w.write("  path_prefix_(std::move(path_prefix)) {}");
    w.dedent();
    w.write("");

    w.openBlock(
        "smithy::Outcome<smithy::http::HttpResponse> $L::Send("
            + "smithy::http::HttpRequest request) const {",
        name);
    w.write("request.headers.Set(\"accept\", $S);", protocol.contentType());
    w.write("request.headers.Set(\"user-agent\", config_.user_agent);");
    w.openBlock("if (!request.body.empty()) {");
    w.write("request.headers.Set(\"content-length\", std::to_string(request.body.size()));");
    w.closeBlock("}");
    w.write("return transport_->Send(request);");
    w.closeBlock("}");
    w.write("");

    for (OperationShape operation : operations()) {
      StructureShape input = ProtocolSupport.inputShape(context, operation);
      String inputType = context.cppSymbols().toSymbol(input).getName();
      String outputType =
          context.cppSymbols().toSymbol(ProtocolSupport.outputShape(context, operation)).getName();
      w.openBlock(
          "smithy::Outcome<$L> $L::$L(const $L& input) const {",
          outputType,
          name,
          CppReservedWords.escape(operation.getId().getName()),
          inputType);
      if (input.members().isEmpty()) {
        w.write("(void)input;");
      }
      protocol.writeOperationBody(w, context, service, operation);
      w.closeBlock("}");
      w.write("");
    }
  }
}
