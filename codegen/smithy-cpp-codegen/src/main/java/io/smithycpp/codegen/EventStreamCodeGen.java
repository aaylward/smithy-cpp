package io.smithycpp.codegen;

import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.TreeMap;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.knowledge.EventStreamIndex;
import software.amazon.smithy.model.knowledge.EventStreamInfo;
import software.amazon.smithy.model.knowledge.HttpBinding;
import software.amazon.smithy.model.knowledge.HttpBindingIndex;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.shapes.UnionShape;
import software.amazon.smithy.model.traits.EventHeaderTrait;
import software.amazon.smithy.model.traits.EventPayloadTrait;

/**
 * Event-stream detection, validation, and emission shared by the client and server generators
 * (ADR-0016): an operation is streaming when its input or output carries an event-stream union
 * (@streaming blobs stay unmodeled — {@link EventStreamIndex} only indexes unions). The runtime
 * owns the envelope (smithy/eventstream/envelope.h); this class emits only the member-name dispatch
 * and serde calls around it — one {@code Encode<Op>Event}/{@code Decode<Op>Event} pair per
 * streaming operation per wire end, with the protocol supplying the payload codec expressions.
 */
final class EventStreamCodeGen {

  private EventStreamCodeGen() {}

  /** Whether the operation has an event-stream union on its input or output. */
  static boolean streaming(Model model, OperationShape operation) {
    EventStreamIndex index = EventStreamIndex.of(model);
    return index.getInputInfo(operation).isPresent() || index.getOutputInfo(operation).isPresent();
  }

  /** The streaming subset of {@code operations}, in their given order. */
  static List<OperationShape> streamingOperations(Model model, List<OperationShape> operations) {
    return operations.stream().filter(op -> streaming(model, op)).toList();
  }

  static Optional<EventStreamInfo> inputInfo(Model model, OperationShape operation) {
    return EventStreamIndex.of(model).getInputInfo(operation);
  }

  static Optional<EventStreamInfo> outputInfo(Model model, OperationShape operation) {
    return EventStreamIndex.of(model).getOutputInfo(operation);
  }

  /**
   * Fails generation for models this slice does not carry (each diagnostic names the shape and the
   * fix): streaming on a refusing protocol, @eventHeader/@eventPayload event members,
   * initial-request members the upgrade request cannot carry, and initial-response members (the
   * ":initial-response" message is a documented deferral).
   */
  static void validate(
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> operations) {
    Model model = context.model();
    EventStreamIndex index = EventStreamIndex.of(model);
    for (OperationShape operation : operations) {
      Optional<EventStreamInfo> input = index.getInputInfo(operation);
      Optional<EventStreamInfo> output = index.getOutputInfo(operation);
      if (input.isEmpty() && output.isEmpty()) {
        continue;
      }
      if (!protocol.supportsEventStreams()) {
        throw new CodegenException(
            "cpp-codegen: "
                + protocol.name()
                + " does not support event-stream operations: "
                + operation.getId()
                + " (bind the service to alloy#simpleRestJson or smithy.protocols#rpcv2Cbor,"
                + " or remove the @streaming union)");
      }
      input.ifPresent(info -> rejectEventBindingTraits(context, info));
      output.ifPresent(info -> rejectEventBindingTraits(context, info));
      if (input.isPresent()) {
        validateInitialRequestMembers(context, protocol, operation, input.get());
      }
      if (output.isPresent() && !output.get().getInitialMessageMembers().isEmpty()) {
        String member = output.get().getInitialMessageMembers().keySet().iterator().next();
        throw new CodegenException(
            "cpp-codegen: streaming operation "
                + operation.getId()
                + " has initial-response member '"
                + member
                + "'; output members outside the stream are not supported yet — move the member"
                + " into an event (the :initial-response message is a documented deferral,"
                + " ADR-0016)");
      }
    }
  }

  /**
   * @eventHeader/@eventPayload are additive refinements of the envelope; rejected for now.
   */
  private static void rejectEventBindingTraits(CppContext context, EventStreamInfo info) {
    for (MemberShape eventMember : eventUnion(info).members()) {
      Shape target = context.model().expectShape(eventMember.getTarget());
      for (MemberShape member : target.members()) {
        if (member.hasTrait(EventHeaderTrait.class)) {
          throw new CodegenException(
              "cpp-codegen: @eventHeader is not supported yet: member "
                  + member.getId()
                  + " (all event members ride the payload document; remove the trait)");
        }
        if (member.hasTrait(EventPayloadTrait.class)) {
          throw new CodegenException(
              "cpp-codegen: @eventPayload is not supported yet: member "
                  + member.getId()
                  + " (all event members ride the payload document; remove the trait)");
        }
      }
    }
  }

  /**
   * Initial-request members must travel on the upgrade request: labels, query, or headers for
   * protocols with @http bindings; nothing at all for fixed-upgrade-URI protocols (ADR-0016 —
   * body-bound initial-request members are a scoping rejection, not a wire constraint).
   */
  private static void validateInitialRequestMembers(
      CppContext context,
      ProtocolGenerator protocol,
      OperationShape operation,
      EventStreamInfo info) {
    if (info.getInitialMessageMembers().isEmpty()) {
      return;
    }
    if (!protocol.bindsInitialRequestMembers()) {
      String member = info.getInitialMessageMembers().keySet().iterator().next();
      throw new CodegenException(
          "cpp-codegen: streaming operation "
              + operation.getId()
              + " has initial-request member '"
              + member
              + "', but "
              + protocol.name()
              + " upgrades on a fixed URI that carries no initial members; remove the member or"
              + " move it into an event");
    }
    Map<String, HttpBinding> bindings =
        HttpBindingIndex.of(context.model()).getRequestBindings(operation);
    for (MemberShape member : info.getInitialMessageMembers().values()) {
      HttpBinding binding = bindings.get(member.getMemberName());
      boolean bound =
          binding != null
              && switch (binding.getLocation()) {
                case LABEL, QUERY, QUERY_PARAMS, HEADER, PREFIX_HEADERS -> true;
                default -> false;
              };
      if (!bound) {
        throw new CodegenException(
            "cpp-codegen: streaming operation "
                + operation.getId()
                + " binds initial-request member '"
                + member.getMemberName()
                + "' to the request body; bind it to a label, query, or header — body-bound"
                + " initial-request members are not supported (ADR-0016)");
      }
    }
  }

  /** The event union a direction models. */
  private static UnionShape eventUnion(EventStreamInfo info) {
    return info.getEventStreamTarget().asUnionShape().orElseThrow();
  }

  private static String eventUnionType(CppContext context, Optional<EventStreamInfo> info) {
    return info.map(i -> context.cppSymbols().toSymbol(eventUnion(i)).getName())
        .orElse("smithy::eventstream::NoEvents");
  }

  /** The client's session type: Tx = the input event union, Rx = the output event union. */
  static String clientStreamType(CppContext context, OperationShape operation) {
    return "smithy::eventstream::EventStream<"
        + eventUnionType(context, inputInfo(context.model(), operation))
        + ", "
        + eventUnionType(context, outputInfo(context.model(), operation))
        + ">";
  }

  /** The handler's session type: the client's with the parameters swapped. */
  static String serverStreamType(CppContext context, OperationShape operation) {
    return "smithy::eventstream::EventStream<"
        + eventUnionType(context, outputInfo(context.model(), operation))
        + ", "
        + eventUnionType(context, inputInfo(context.model(), operation))
        + ">";
  }

  private static String opName(OperationShape operation) {
    return CppReservedWords.escape(operation.getId().getName());
  }

  /**
   * The streaming override a generated test handler (smoke/integration) emits: streaming operations
   * are skipped by the unary-shaped test harnesses — a blocking round trip has no meaning for a
   * session, and their transports never upgrade — but the handler subclass still needs every
   * pure-virtual method defined, so each streaming operation gets this close-immediately stub.
   */
  static void writeTestHandlerStub(CppWriter w, CppContext context, OperationShape operation) {
    String inputType =
        context.cppSymbols().toSymbol(ProtocolSupport.inputShape(context, operation)).getName();
    w.write("// Streaming operation (ADR-0016): no generated unary-shaped test drives");
    w.write("// this; the stub closes the stream so the interface stays implemented.");
    w.openBlock(
        "smithy::Outcome<smithy::Unit> $L(const $L& input, $L& stream, $L) override {",
        opName(operation),
        inputType,
        serverStreamType(context, operation),
        ProtocolSupport.REQUEST_CONTEXT_PARAM);
    w.write("(void)input;");
    w.write("stream.Close();");
    w.write("return smithy::Unit{};");
    w.closeBlock("}");
  }

  /**
   * Emits the client-side stream support into client.cc's anonymous namespace: the DialStream
   * helper and one Encode/Decode pair per streaming operation. Must run after {@link
   * ProtocolSupport#writeOperationErrorParsers} — the decoders dispatch exceptions through the
   * Make&lt;Error&gt;Error functions it emits.
   */
  static void writeClientStreamHelpers(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> streamingOperations,
      String clientName) {
    w.addInclude("\"smithy/eventstream/envelope.h\"");
    w.addInclude("\"smithy/eventstream/event_stream.h\"");
    w.addInclude("\"smithy/http/websocket.h\"");
    w.addInclude("<memory>");
    w.write("// Dials the WebSocket a streaming operation rides (ADR-0016): host, port, and");
    w.write("// TLS come from the same endpoint the unary transport uses (nothing is");
    w.write("// configured twice); config.websocket_dialer overrides the Beast dialer the");
    w.write("// way http_client overrides the unary transport.");
    w.openBlock(
        "smithy::Outcome<std::shared_ptr<smithy::http::WebSocket>> DialStream("
            + "const smithy::ClientConfig& config, smithy::http::WebSocketDialRequest request) {");
    w.openBlock("if (!config.endpoint.empty()) {");
    w.write("auto endpoint = smithy::http::ParseEndpoint(config.endpoint);");
    w.write("if (!endpoint) return std::move(endpoint).error();");
    w.write("request.host = endpoint->host;");
    w.write("request.port = endpoint->port;");
    w.write("request.tls = endpoint->tls();");
    w.write("request.tls_options = config.tls;");
    w.closeBlock("}");
    w.write("if (config.websocket_dialer) return config.websocket_dialer(request);");
    w.openBlock("if (request.host.empty()) {");
    w.write(
        "return smithy::Error::Validation($S);",
        clientName + ": config needs an endpoint or a websocket_dialer");
    w.closeBlock("}");
    w.write("return smithy::http::BeastWebSocketClient::Dialer()(request);");
    w.closeBlock("}");
    w.write("");
    for (OperationShape operation : streamingOperations) {
      writeEncodeFunction(w, context, protocol, operation, inputInfo(context.model(), operation));
      writeDecodeFunction(
          w,
          context,
          service,
          protocol,
          operation,
          outputInfo(context.model(), operation),
          /* clientSide= */ true);
    }
  }

  /**
   * Emits the server-side stream support into server.cc's anonymous namespace: per streaming
   * operation, the Encode/Decode pair (directions swapped) and the exception-message builder a
   * failed handler's error goes out through.
   */
  static void writeServerStreamHelpers(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> streamingOperations) {
    w.addInclude("\"smithy/eventstream/envelope.h\"");
    w.addInclude("\"smithy/http/websocket.h\"");
    for (OperationShape operation : streamingOperations) {
      writeEncodeFunction(w, context, protocol, operation, outputInfo(context.model(), operation));
      writeDecodeFunction(
          w,
          context,
          service,
          protocol,
          operation,
          inputInfo(context.model(), operation),
          /* clientSide= */ false);
      writeExceptionMessageBuilder(w, context, service, protocol, operation);
    }
  }

  /**
   * The shared tail of every streaming operation method: dial with the prepared {@code request},
   * then wrap the session in the operation's typed EventStream over its codec pair.
   */
  static void writeDialAndReturn(CppWriter w, CppContext context, OperationShape operation) {
    String op = opName(operation);
    w.write("auto socket = DialStream(config_, std::move(request));");
    w.write("if (!socket) return std::move(socket).error();");
    w.write(
        "return $L(*std::move(socket), Encode$LEvent, Decode$LEvent);",
        clientStreamType(context, operation),
        op,
        op);
  }

  /** Encode&lt;Op&gt;Event over the direction's union (or the never-called NoEvents slot). */
  private static void writeEncodeFunction(
      CppWriter w,
      CppContext context,
      ProtocolGenerator protocol,
      OperationShape operation,
      Optional<EventStreamInfo> tx) {
    String op = opName(operation);
    if (tx.isEmpty()) {
      w.write("// $L models no events in this direction; the codec slot is filled but", op);
      w.write("// never invoked (nothing constructs a smithy::eventstream::NoEvents).");
      w.openBlock(
          "smithy::Outcome<smithy::eventstream::Message> Encode$LEvent("
              + "const smithy::eventstream::NoEvents&) {",
          op);
      w.write(
          "return smithy::Error::Validation($S);",
          op + ": no events are modeled in this direction");
      w.closeBlock("}");
      w.write("");
      return;
    }
    UnionShape union = eventUnion(tx.get());
    String unionType = context.cppSymbols().toSymbol(union).getName();
    w.write("// One event per message (ADR-0016): the engaged member's structure is the");
    w.write("// payload, its member name the :event-type.");
    w.openBlock(
        "smithy::Outcome<smithy::eventstream::Message> Encode$LEvent(const $L& event) {",
        op,
        unionType);
    for (MemberShape member : union.members()) {
      String name = context.cppSymbols().toMemberName(member);
      Shape target = context.model().expectShape(member.getTarget());
      w.openBlock("if (event.is_$L()) {", name);
      w.write(
          "return smithy::eventstream::MakeEventMessage($S, $S, $L);",
          member.getMemberName(),
          protocol.contentType(),
          protocol.eventPayloadEncode(
              "Serialize"
                  + SerdeCodeGen.serdeFunctionSuffix(context, target)
                  + "(event.as_"
                  + name
                  + "())"));
      w.closeBlock("}");
    }
    w.write("return smithy::Error::Validation($S);", unionType + ": no event member engaged");
    w.closeBlock("}");
    w.write("");
  }

  /**
   * Decode&lt;Op&gt;Event: envelope parse, exception handling (terminal, ADR-0016), then
   * member-name dispatch into the union's From factories. The client end resolves exceptions
   * through the operation's Make&lt;Error&gt;Error machinery; a server receiving one (clients never
   * send exceptions) reports a terminal protocol violation.
   */
  private static void writeDecodeFunction(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      OperationShape operation,
      Optional<EventStreamInfo> rx,
      boolean clientSide) {
    String op = opName(operation);
    if (rx.isEmpty()) {
      w.write("// $L models no events in this direction: any received message is a", op);
      w.write("// protocol violation and therefore terminal (ADR-0016).");
      w.openBlock(
          "smithy::Outcome<smithy::eventstream::NoEvents> Decode$LEvent("
              + "const smithy::eventstream::Message&) {",
          op);
      w.write(
          "return smithy::Error::Serialization($S);",
          op + ": no events are modeled in this direction");
      w.closeBlock("}");
      w.write("");
      return;
    }
    UnionShape union = eventUnion(rx.get());
    String unionType = context.cppSymbols().toSymbol(union).getName();
    w.openBlock(
        "smithy::Outcome<$L> Decode$LEvent(const smithy::eventstream::Message& message) {",
        unionType,
        op);
    w.write("auto envelope = smithy::eventstream::ParseEnvelope(message);");
    w.write("if (!envelope) return std::move(envelope).error();");
    w.openBlock("if (envelope->kind == smithy::eventstream::EventEnvelope::Kind::kException) {");
    if (clientSide) {
      writeClientExceptionDecode(w, context, service, protocol, operation);
    } else {
      w.write("// Clients send events, never exceptions; treat one as a terminal protocol");
      w.write("// violation carrying the peer's claimed identity.");
      w.write("return smithy::Error::Modeled(envelope->type, \"peer sent an exception message\");");
    }
    w.closeBlock("}");
    for (MemberShape member : union.members()) {
      Shape target = context.model().expectShape(member.getTarget());
      w.openBlock("if (envelope->type == $S) {", member.getMemberName());
      w.write("auto doc = $L;", protocol.eventPayloadDecode("envelope->payload"));
      w.write("if (!doc) return std::move(doc).error();");
      w.write(
          "auto event = Deserialize$L(*doc);", SerdeCodeGen.serdeFunctionSuffix(context, target));
      w.write("if (!event) return std::move(event).error();");
      w.write(
          "return $L::From$L(*std::move(event));",
          unionType,
          SerdeGenerator.pascal(context.cppSymbols().toMemberName(member)));
      w.closeBlock("}");
    }
    w.write(
        "return smithy::Error::Serialization($S + envelope->type);", op + ": unknown event type: ");
    w.closeBlock("}");
    w.write("");
  }

  /**
   * The client end of a received exception: surface it exactly like a unary modeled error —
   * Error::Modeled(code, message) with the typed detail attached when the :exception-type matches a
   * declared error shape, the generic fallback otherwise.
   */
  private static void writeClientExceptionDecode(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      OperationShape operation) {
    w.write("// A received exception is terminal (ADR-0016): the EventStream closes the");
    w.write("// session and surfaces this error, exactly the unary shape.");
    w.write("ParsedError parsed;");
    w.write("parsed.code = SanitizeErrorCode(envelope->type);");
    w.write("auto exception_doc = $L;", protocol.eventPayloadDecode("envelope->payload"));
    w.openBlock("if (exception_doc.ok() && exception_doc->is_map()) {");
    w.write("parsed.doc = *std::move(exception_doc);");
    w.write("const smithy::Document* text = parsed.doc.Find(\"message\");");
    w.write("if (text != nullptr && text->is_string()) parsed.message = text->as_string();");
    w.closeBlock("}");
    Map<String, StructureShape> errors = new TreeMap<>();
    for (ShapeId errorId : operation.getErrors(service)) {
      StructureShape shape = context.model().expectShape(errorId).asStructureShape().orElseThrow();
      errors.put(context.cppSymbols().toSymbol(shape).getName(), shape);
    }
    if (!errors.isEmpty()) {
      w.write("// Make<Error>Error's header-patch source; exception messages carry none.");
      w.write("smithy::http::HttpResponse response;");
      for (Map.Entry<String, StructureShape> entry : errors.entrySet()) {
        w.write(
            "if (parsed.code == $S) return Make$LError(response, std::move(parsed));",
            entry.getValue().getId().getName(),
            entry.getKey());
      }
    }
    w.write("return GenericError(std::move(parsed));");
  }

  /**
   * Build&lt;Op&gt;ExceptionMessage: the exception message a handler failure sends before the
   * close, mirroring unary ErrorToResponse's identity mapping (modeled code + serialized detail;
   * SerializationException for validation/serialization failures; a non-leaking InternalFailure
   * otherwise).
   */
  private static void writeExceptionMessageBuilder(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      OperationShape operation) {
    String op = opName(operation);
    Map<String, StructureShape> errors = new TreeMap<>();
    for (ShapeId errorId : operation.getErrors(service)) {
      StructureShape shape = context.model().expectShape(errorId).asStructureShape().orElseThrow();
      errors.put(context.cppSymbols().toSymbol(shape).getName(), shape);
    }
    w.write("// A handler failure ends the stream with one exception message before the");
    w.write("// close (ADR-0016).");
    w.openBlock(
        "smithy::eventstream::Message Build$LExceptionMessage(const smithy::Error& error) {", op);
    w.write("std::string type = \"InternalFailure\";");
    w.write("// Never leak internal detail on unexpected failures.");
    w.write("std::string message = \"internal failure\";");
    w.write("smithy::DocumentMap body;");
    w.openBlock("if (error.kind() == smithy::ErrorKind::kModeled) {");
    w.write("type = error.code();");
    w.write("message = error.message();");
    for (Map.Entry<String, StructureShape> entry : errors.entrySet()) {
      w.openBlock("if (error.code() == $S) {", entry.getValue().getId().getName());
      w.openBlock("if (const auto* detail = error.detail<$L>()) {", entry.getKey());
      w.write(
          "body = Serialize$L(*detail).as_map();",
          SerdeCodeGen.serdeFunctionSuffix(context, entry.getValue()));
      w.closeBlock("}");
      w.closeBlock("}");
    }
    w.closeBlock(
        "} else if (error.kind() == smithy::ErrorKind::kValidation || error.kind() =="
            + " smithy::ErrorKind::kSerialization) {");
    w.indent();
    w.write("type = \"SerializationException\";");
    w.write("message = error.message();");
    w.closeBlock("}");
    w.write("// The typed detail's own message member wins over the generic one.");
    w.write(
        "const bool has_message = body.count(\"message\") != 0 || body.count(\"Message\") != 0;");
    w.openBlock("if (!has_message && !message.empty()) {");
    w.write("body.emplace(\"message\", smithy::Document(std::move(message)));");
    w.closeBlock("}");
    w.write(
        "return smithy::eventstream::MakeExceptionMessage(type, $S, $L);",
        protocol.contentType(),
        protocol.eventPayloadEncode("smithy::Document(std::move(body))"));
    w.closeBlock("}");
    w.write("");
  }
}
