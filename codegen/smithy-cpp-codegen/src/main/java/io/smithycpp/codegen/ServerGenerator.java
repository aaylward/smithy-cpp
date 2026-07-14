package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.DocumentationTrait;

/**
 * Emits server.h / src/server.cc: the pure-virtual {@code <Service>Handler} interface users
 * implement and the {@code <Service>Server} that binds it to the runtime router. Transport agnostic
 * — the resulting {@code smithy::http::RequestHandler} plugs into any {@code HttpServerTransport}
 * (Loopback, SocketHttpServer, BeastServerTransport).
 */
final class ServerGenerator {

  private final CppContext context;
  private final ServiceShape service;
  private final ProtocolGenerator protocol;
  private final List<OperationShape> operations;

  ServerGenerator(
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> operations) {
    this.context = context;
    this.service = service;
    this.protocol = protocol;
    this.operations = operations;
  }

  private String serviceName() {
    return CppReservedWords.escape(service.getId().getName());
  }

  void run() {
    rejectRouteConflicts();
    context
        .writerDelegator()
        .useFileWriter(context.settings().serverHeaderFile(), this::writeHeader);
    context.writerDelegator().useFileWriter("src/server.cc", this::writeSource);
  }

  /**
   * Two operations whose method + URI pattern shape coincide (labels are interchangeable for
   * matching purposes) could never be routed apart; fail generation instead of emitting a server
   * that silently drops one of them.
   */
  private void rejectRouteConflicts() {
    java.util.Map<String, OperationShape> seen = new java.util.HashMap<>();
    for (OperationShape operation : operations) {
      var http = operation.getTrait(software.amazon.smithy.model.traits.HttpTrait.class);
      if (http.isEmpty()) {
        continue; // rpcv2Cbor routes are keyed by operation name and cannot collide.
      }
      StringBuilder shape = new StringBuilder(http.get().getMethod());
      for (var segment : http.get().getUri().getSegments()) {
        shape.append('/');
        if (segment.isGreedyLabel()) {
          shape.append("{+}");
        } else if (segment.isLabel()) {
          shape.append("{}");
        } else {
          shape.append(segment.getContent());
        }
      }
      OperationShape existing = seen.putIfAbsent(shape.toString(), operation);
      if (existing != null) {
        throw new software.amazon.smithy.codegen.core.CodegenException(
            "cpp-codegen: ambiguous routes: "
                + existing.getId()
                + " and "
                + operation.getId()
                + " share the route shape '"
                + shape
                + "'");
      }
    }
  }

  private void writeHeader(CppWriter w) {
    w.addInclude("<memory>");
    w.addInclude("\"" + context.settings().includePrefix() + "/types.h\"");
    w.addInclude("\"smithy/core/outcome.h\"");
    w.addInclude("\"smithy/http/transport.h\"");
    w.addInclude("\"smithy/server/router.h\"");

    String name = serviceName();
    w.write("/// Implement one method per operation. Return a modeled error as");
    w.write("/// smithy::Error::Modeled(\"<ErrorShapeName>\", message), optionally with the");
    w.write("/// typed error structure attached via set_detail() so it serializes fully.");
    w.write("/// Implementations must be thread-safe: transports may invoke any mix of");
    w.write("/// operations concurrently on the one handler instance.");
    w.openBlock("class $LHandler {", name);
    w.write("public:").indent();
    w.write("virtual ~$LHandler() = default;", name);
    w.write("");
    for (OperationShape operation : operations) {
      operation
          .getTrait(DocumentationTrait.class)
          .ifPresent(
              docs -> {
                for (String line : docs.getValue().split("\n", -1)) {
                  w.write("/// $L", line);
                }
              });
      StructureShape input = ProtocolSupport.inputShape(context, operation);
      StructureShape output = ProtocolSupport.outputShape(context, operation);
      w.write(
          "virtual smithy::Outcome<$L> $L(const $L& input) = 0;",
          context.cppSymbols().toSymbol(output).getName(),
          CppReservedWords.escape(operation.getId().getName()),
          context.cppSymbols().toSymbol(input).getName());
    }
    w.dedent();
    w.closeBlock("};");
    w.write("");
    w.write(
        "/// $L server for $L: routing, deserialization, handler dispatch,",
        protocol.name(),
        service.getId().toString());
    w.write("/// response serialization, and modeled-error mapping. Pass Handler() to any");
    w.write("/// smithy::http::HttpServerTransport.");
    w.openBlock("class $LServer {", name);
    w.write("public:").indent();
    w.write("explicit $LServer(std::shared_ptr<$LHandler> handler);", name, name);
    w.write("");
    w.write("smithy::http::RequestHandler Handler() const;");
    w.write("").dedent();
    w.write("private:").indent();
    w.write("std::shared_ptr<smithy::server::Router> router_;").dedent();
    w.closeBlock("};");
    w.write("");
  }

  private void writeSource(CppWriter w) {
    for (String include : ProtocolSupport.sharedServerIncludes(context)) {
      w.addInclude(include);
    }
    for (String include : protocol.serverIncludes()) {
      w.addInclude(include);
    }
    String name = serviceName();

    w.write("namespace {");
    w.write("");
    protocol.writeServerHelpers(w, context, service, operations);
    w.write("}  // namespace");
    w.write("");

    w.openBlock("$LServer::$LServer(std::shared_ptr<$LHandler> handler)", name, name, name);
    w.write(": router_(std::make_shared<smithy::server::Router>()) {");
    w.dedent();
    w.indent();
    w.write("// The route table is derived from the model's @http traits; conflicts are");
    w.write("// a modeling error surfaced by Router::Add (checked at generation time in a");
    w.write("// later phase), so registration results are intentionally discarded.");
    protocol.writeServerRoutes(w, context, service, operations);
    w.dedent();
    w.write("}");
    w.write("");

    w.openBlock("smithy::http::RequestHandler $LServer::Handler() const {", name);
    w.write("auto router = router_;");
    w.write(
        "return [router](const smithy::http::HttpRequest& request) "
            + "{ return router->Route(request); };");
    w.closeBlock("}");
    w.write("");
  }
}
