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

  /** The event-streaming subset (ADR-0016); empty for unary-only services. */
  private List<OperationShape> streamingOperations() {
    return EventStreamCodeGen.streamingOperations(context.model(), operations);
  }

  /** The unary complement of {@link #streamingOperations()}. */
  private List<OperationShape> unaryOperations() {
    return operations.stream()
        .filter(op -> !EventStreamCodeGen.streaming(context.model(), op))
        .toList();
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
    boolean hasStreaming = !streamingOperations().isEmpty();
    if (hasStreaming) {
      w.addInclude("\"smithy/eventstream/event_stream.h\"");
      w.addInclude("\"smithy/server/websocket_router.h\"");
    }

    String name = serviceName();
    w.write("/// Implement one method per operation. Return a modeled error as");
    w.write("/// smithy::Error::Modeled(\"<ErrorShapeName>\", message), optionally with the");
    w.write("/// typed error structure attached via set_detail() so it serializes fully.");
    w.write("/// The context carries the raw request and routing captures — see");
    w.write("/// smithy::server::RequestContext; leave the parameter unnamed when unused.");
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
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        // Streaming operations (ADR-0016): input first, context last, the
        // ADR-0010 shape, with the borrowed session in between.
        w.write("/// Streaming operation (ADR-0016): Send/Receive on `stream` until done,");
        w.write("/// then return Unit for a clean close — or an error, which ends the");
        w.write("/// stream with an exception message before the close (unless the");
        w.write("/// stream already terminated: propagating a failed Receive() closes");
        w.write("/// without a message — the peer already observed that failure).");
        w.write("/// `stream` is valid only until this method returns; join any helper");
        w.write("/// thread still using it. Blocks the transport's handler thread for");
        w.write("/// the session's lifetime.");
        w.write(
            "virtual smithy::Outcome<smithy::Unit> $L(const $L& input, $L& stream, $L context)"
                + " = 0;",
            CppReservedWords.escape(operation.getId().getName()),
            context.cppSymbols().toSymbol(input).getName(),
            EventStreamCodeGen.serverStreamType(context, operation),
            ProtocolSupport.REQUEST_CONTEXT_PARAM);
        continue;
      }
      w.write(
          "virtual smithy::Outcome<$L> $L(const $L& input, $L context) = 0;",
          context.cppSymbols().toSymbol(output).getName(),
          CppReservedWords.escape(operation.getId().getName()),
          context.cppSymbols().toSymbol(input).getName(),
          ProtocolSupport.REQUEST_CONTEXT_PARAM);
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
    if (hasStreaming) {
      w.write("");
      w.write("/// The WebSocket router carrying every streaming route (ADR-0016), ready");
      w.write("/// to mount on the transport in two lines:");
      w.write("///   options.websocket_gate = server.StreamRouter()->Gate();");
      w.write("///   options.on_websocket = server.StreamRouter()->Serve();");
      w.write("std::shared_ptr<smithy::server::WebSocketRouter> StreamRouter() const;");
    }
    w.write("").dedent();
    w.write("private:").indent();
    w.write("std::shared_ptr<smithy::server::Router> router_;");
    if (hasStreaming) {
      w.write("std::shared_ptr<smithy::server::WebSocketRouter> stream_router_;");
    }
    w.dedent();
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
    boolean hasStreaming = !streamingOperations().isEmpty();

    w.write("namespace {");
    w.write("");
    protocol.writeServerHelpers(w, context, service, operations);
    if (hasStreaming) {
      EventStreamCodeGen.writeServerStreamHelpers(
          w, context, service, protocol, streamingOperations());
    }
    w.write("}  // namespace");
    w.write("");

    w.openBlock("$LServer::$LServer(std::shared_ptr<$LHandler> handler)", name, name, name);
    if (hasStreaming) {
      w.write(": router_(std::make_shared<smithy::server::Router>()),");
      w.write("  stream_router_(std::make_shared<smithy::server::WebSocketRouter>()) {");
    } else {
      w.write(": router_(std::make_shared<smithy::server::Router>()) {");
    }
    w.dedent();
    w.indent();
    w.write("// The route table is derived from the model's @http traits; conflicts are");
    w.write("// a modeling error surfaced by Router::Add (checked at generation time in a");
    w.write("// later phase), so registration results are intentionally discarded.");
    protocol.writeServerRoutes(w, context, service, unaryOperations());
    if (hasStreaming) {
      w.write("// Streaming routes (ADR-0016) live on the WebSocket router; the upgrade");
      w.write("// path bypasses the HTTP chain (ADR-0015), so they never collide with");
      w.write("// the unary table above.");
      for (OperationShape operation : streamingOperations()) {
        protocol.writeStreamServerRoute(w, context, service, operation);
      }
    }
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

    if (hasStreaming) {
      w.openBlock(
          "std::shared_ptr<smithy::server::WebSocketRouter> $LServer::StreamRouter() const {",
          name);
      w.write("return stream_router_;");
      w.closeBlock("}");
      w.write("");
    }
  }
}
