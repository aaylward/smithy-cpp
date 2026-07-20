package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;
import software.amazon.smithy.codegen.core.CodegenException;

/**
 * The generated event-stream surface (ADR-0016): streaming detection, the client and server
 * signatures and codec bodies, the WebSocket routes, the scope diagnostics, and the BUILD dep
 * gating. String-contains pins over {@link PluginTestHarness} runs, like the sibling suites.
 */
class EventStreamGeneratorTest {

  /**
   * A bidirectional chat-like operation with every allowed initial-request binding, plus a
   * server-push one (no input stream — the NoEvents direction). POST on the bidi operation: the
   * modeled method is free (upgrades are GETs on the wire), and GET plus @httpPayload trips
   * Smithy's own HttpMethodSemantics danger at assembly.
   */
  private static final String REST_MODEL =
      """
      $version: "2.0"
      namespace test.stream
      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Converse, Watch] }

      @http(method: "POST", uri: "/rooms/{room}/converse")
      operation Converse {
          input := {
              @required
              @httpLabel
              room: String

              @httpQuery("since")
              since: Integer

              @httpHeader("x-client")
              client: String

              @httpPayload
              events: ClientEvents
          }
          output := {
              @httpPayload
              events: ServerEvents
          }
          errors: [RoomGone]
      }

      @readonly
      @http(method: "GET", uri: "/watch")
      operation Watch {
          output := {
              @httpPayload
              events: ServerEvents
          }
      }

      @streaming
      union ClientEvents {
          message: ChatMessage
      }

      @streaming
      union ServerEvents {
          message: ChatMessage
          joined: MemberJoined
      }

      structure ChatMessage { @required text: String }
      structure MemberJoined { @required member: String }

      @error("client")
      @httpError(410)
      structure RoomGone { message: String }
      """;

  /** The rpcv2Cbor face: stream-only inputs, since the fixed URI carries no initial members. */
  private static final String CBOR_MODEL =
      """
      $version: "2.0"
      namespace test.stream
      use smithy.protocols#rpcv2Cbor

      @rpcv2Cbor
      service Svc { version: "1", operations: [Chat] }

      operation Chat {
          input := { events: ClientEvents }
          output := { events: ServerEvents }
          errors: [RoomGone]
      }

      @streaming
      union ClientEvents { message: ChatMessage }

      @streaming
      union ServerEvents { message: ChatMessage }

      structure ChatMessage { @required text: String }

      @error("client")
      structure RoomGone { message: String }
      """;

  private static MockManifest rest() {
    return PluginTestHarness.generate(REST_MODEL, "test.stream#Svc", "test::stream");
  }

  private static MockManifest cbor() {
    return PluginTestHarness.generate(CBOR_MODEL, "test.stream#Svc", "test::stream");
  }

  @Test
  void clientSignaturesCarryTheTypedSessionPerDirection() {
    String client = rest().expectFileString("/include/test/stream/client.h");
    assertTrue(client.contains("#include \"smithy/eventstream/event_stream.h\""), client);
    // One named alias per streaming operation; the signatures use it, so
    // consumers never respell the two-parameter template.
    assertTrue(
        client.contains(
            "using ConverseClientStream ="
                + " smithy::eventstream::EventStream<ClientEvents, ServerEvents>;"),
        client);
    assertTrue(
        client.contains(
            "smithy::Outcome<ConverseClientStream> Converse(const ConverseInput& input) const;"),
        client);
    // Detection is directional: no input stream parameterizes Tx with the
    // runtime's NoEvents (and the empty input still defaults).
    assertTrue(
        client.contains(
            "using WatchClientStream ="
                + " smithy::eventstream::EventStream<smithy::eventstream::NoEvents,"
                + " ServerEvents>;"),
        client);
    assertTrue(
        client.contains(
            "smithy::Outcome<WatchClientStream> Watch(const WatchInput& input = {}) const;"),
        client);
    // The NoEvents direction's doc-comment tells the truth: Send does not
    // compile there, only Receive is meaningful.
    assertTrue(client.contains("models no client-to-server events"), client);
  }

  @Test
  void clientBodyDialsTheUpgradeAndSuppliesTheCodecs() {
    String client = rest().expectFileString("/src/client.cc");
    // The upgrade target resolves labels and query exactly like a unary
    // request; header bindings ride the upgrade GET.
    assertTrue(client.contains("target += smithy::http::EncodePathSegment(input.room);"), client);
    assertTrue(client.contains("query.Add(\"since\","), client);
    assertTrue(client.contains("request.headers.Set(\"x-client\", (*input.client));"), client);
    // config.websocket_dialer wins; the Beast dialer is the fallback.
    assertTrue(
        client.contains("if (config.websocket_dialer) return config.websocket_dialer(request);"),
        client);
    assertTrue(
        client.contains("return smithy::http::BeastWebSocketClient::Dialer()(request);"), client);
    // Encode: member-name dispatch -> serde -> protocol bytes -> envelope.
    assertTrue(
        client.contains(
            "return smithy::eventstream::MakeEventMessage(\"message\", \"application/json\","
                + " smithy::Blob::FromString(smithy::json::Encode(SerializeChatMessage("
                + "event.as_message()))));"),
        client);
    // Decode: envelope parse, exception dispatch through the Make<Error>Error
    // machinery (generic fallback), then member-name dispatch into the union.
    assertTrue(client.contains("smithy::eventstream::ParseEnvelope(message);"), client);
    assertTrue(
        client.contains(
            "if (parsed.code == \"RoomGone\") return MakeRoomGoneError(response,"
                + " std::move(parsed));"),
        client);
    assertTrue(client.contains("return GenericError(std::move(parsed));"), client);
    assertTrue(client.contains("return ServerEvents::FromJoined(*std::move(event));"), client);
    assertTrue(client.contains("\"Converse: unknown event type: \" + envelope->type"), client);
    // Streaming operations never parse an HTTP error response, so the unary
    // Parse<Op>Error dispatcher must not be emitted for them (dead code).
    assertFalse(client.contains("ParseConverseError"), client);
    // The NoEvents transmit direction gets no encoder stub: Send is a
    // compile error there (event_stream.h), so the slot rides empty.
    assertFalse(client.contains("EncodeWatchEvent"), client);
    assertTrue(
        client.contains("return WatchClientStream(*std::move(socket), {}, DecodeWatchEvent);"),
        client);
  }

  @Test
  void serverGrowsStreamingHandlersAndAStreamRouter() {
    MockManifest manifest = rest();
    String header = manifest.expectFileString("/include/test/stream/server.h");
    assertTrue(
        header.contains(
            "using ConverseServerStream ="
                + " smithy::eventstream::EventStream<ServerEvents, ClientEvents>;"),
        header);
    assertTrue(
        header.contains(
            "virtual smithy::Outcome<smithy::Unit> Converse(const ConverseInput& input,"
                + " ConverseServerStream& stream,"
                + " const smithy::server::RequestContext& context) = 0;"),
        header);
    assertTrue(
        header.contains("std::shared_ptr<smithy::server::WebSocketRouter> StreamRouter() const;"),
        header);
    assertTrue(
        header.contains("std::shared_ptr<smithy::server::WebSocketRouter> stream_router_;"),
        header);

    String server = manifest.expectFileString("/src/server.cc");
    // Streaming routes register as GET (upgrades are GETs on the wire,
    // whatever the operation models) on the WebSocket router.
    assertTrue(
        server.contains("(void)stream_router_->Add(\"GET\", \"/rooms/{room}/converse\","), server);
    assertTrue(server.contains("auto input = ParseConverseInput(request, context,"), server);
    assertTrue(server.contains("handler->Converse(*input, stream, context);"), server);
    // A handler failure sends the exception message, then the close.
    assertTrue(
        server.contains("(void)socket.Send(BuildConverseExceptionMessage(outcome.error()));"),
        server);
    assertTrue(server.contains("stream.Close();"), server);
    assertTrue(server.contains("return smithy::eventstream::MakeExceptionMessage(type,"), server);
    // Streaming operations answer over the session, never an HTTP response.
    assertFalse(server.contains("BuildConverseResponse"), server);
    assertFalse(server.contains("BuildWatchResponse"), server);
    // Nothing in Svc is constrained, so the stream routes carry no
    // validation refusal — the unary writeRoute guards, mirrored (dead
    // emission stays dead; see streamRoutesGuardValidationLikeUnaryRoutes
    // for the constrained flip side).
    assertFalse(server.contains("if (!validation_failures.empty())"), server);
    // The server's Watch receive direction is NoEvents: the decode stub
    // stays (a message received there is a real, reachable protocol
    // violation), while its transmit direction still encodes for real.
    assertTrue(server.contains("Watch: no events are modeled in this direction"), server);
    assertTrue(
        server.contains("WatchServerStream stream(socket, EncodeWatchEvent, DecodeWatchEvent);"),
        server);
  }

  @Test
  void serverGrowsTheAsyncHandlerAndSessionRoutes() {
    // ADR-0021: the coroutine sibling — async aliases, the StreamTask
    // handler, the second constructor wiring AddSession launch points, and
    // the Detached wrapper that frames a failed outcome via SendAsync.
    MockManifest manifest = rest();
    String header = manifest.expectFileString("/include/test/stream/server.h");
    assertTrue(
        header.contains(
            "using ConverseAsyncServerStream ="
                + " smithy::eventstream::AsyncEventStream<ServerEvents, ClientEvents>;"),
        header);
    assertTrue(
        header.contains(
            "virtual smithy::eventstream::StreamTask Converse(ConverseInput input,"
                + " ConverseAsyncServerStream& stream) = 0;"),
        header);
    // Unary operations keep the blocking shape on the async handler.
    assertTrue(header.contains("class SvcAsyncHandler {"), header);
    assertTrue(
        header.contains("explicit SvcServer(std::shared_ptr<SvcAsyncHandler> handler);"), header);

    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(
        server.contains("(void)stream_router_->AddSession(\"GET\", \"/rooms/{room}/converse\","),
        server);
    // The launch point parses like the blocking route, refuses on the owned
    // socket, and hands off to the wrapper without blocking.
    assertTrue(
        server.contains("(void)socket->Send(BuildConverseExceptionMessage(input.error()));"),
        server);
    assertTrue(
        server.contains("ServeConverseAsync(handler, *std::move(input), std::move(socket));"),
        server);
    assertTrue(
        server.contains(
            "smithy::eventstream::Detached ServeConverseAsync(std::shared_ptr<SvcAsyncHandler>"
                + " handler, ConverseInput input,"
                + " std::shared_ptr<smithy::http::WebSocket> socket) {"),
        server);
    assertTrue(
        server.contains("auto outcome = co_await handler->Converse(std::move(input), stream);"),
        server);
    // A failed outcome is framed through SendAsync — never a blocking Send
    // on a completion context — and the close rides its callback.
    assertTrue(
        server.contains("socket->SendAsync(BuildConverseExceptionMessage(outcome.error()),"),
        server);
    assertTrue(
        server.contains("[socket](const smithy::Outcome<smithy::Unit>&) { socket->Close(); });"),
        server);
  }

  @Test
  void streamRoutesGuardValidationLikeUnaryRoutes() {
    // The flip side of the absence pin above: with a constrained initial
    // member, the constrained operation's route validates and refuses over
    // the session (one SerializationException-shaped exception message,
    // then the close); the unconstrained neighbor gets the parse-level
    // check only, never a validator call — exactly writeRoute's guards.
    String model = REST_MODEL.replace("@httpLabel\n", "@httpLabel\n        @length(max: 8)\n");
    String server =
        PluginTestHarness.generate(model, "test.stream#Svc", "test::stream")
            .expectFileString("/src/server.cc");
    assertTrue(
        server.contains("ValidateConverseInput(*input, \"\", &validation_failures);"), server);
    assertTrue(
        server.contains(
            "(void)socket.Send(BuildConverseExceptionMessage("
                + "smithy::Error::Validation(validation_failures.front().message)));"),
        server);
    assertFalse(server.contains("ValidateWatchInput"), server);
  }

  @Test
  void rpcv2CborStreamsOnTheFixedUpgradeUriWithCborPayloads() {
    MockManifest manifest = cbor();
    String client = manifest.expectFileString("/src/client.cc");
    assertTrue(
        client.contains("request.target = path_prefix_ + \"/service/Svc/operation/Chat\";"),
        client);
    assertTrue(
        client.contains(
            "return smithy::eventstream::MakeEventMessage(\"message\", \"application/cbor\","
                + " smithy::cbor::Encode(SerializeChatMessage(event.as_message())));"),
        client);
    assertTrue(client.contains("smithy::cbor::Decode(envelope->payload)"), client);
    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(
        server.contains("(void)stream_router_->Add(\"GET\", \"/service/Svc/operation/Chat\","),
        server);
    assertTrue(server.contains("handler->Chat(input, stream, context);"), server);
  }

  @Test
  void jsonRpc2RefusesEventStreams() {
    String model =
        """
        $version: "2.0"
        namespace test.stream
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Chat] }

        operation Chat {
            input := { events: Events }
        }

        @streaming
        union Events { message: ChatMessage }

        structure ChatMessage { @required text: String }
        """;
    var error =
        assertThrows(
            CodegenException.class,
            () -> PluginTestHarness.generate(model, "test.stream#Svc", "test::stream"));
    assertTrue(error.getMessage().contains("cpp-codegen"), error.getMessage());
    assertTrue(error.getMessage().contains("jsonRpc2"), error.getMessage());
    assertTrue(error.getMessage().contains("test.stream#Chat"), error.getMessage());
  }

  @Test
  void eventHeaderIsRejected() {
    String model =
        CBOR_MODEL.replace(
            "structure ChatMessage { @required text: String }",
            "structure ChatMessage { @eventHeader id: String, @required text: String }");
    var error =
        assertThrows(
            CodegenException.class,
            () -> PluginTestHarness.generate(model, "test.stream#Svc", "test::stream"));
    assertTrue(error.getMessage().contains("cpp-codegen"), error.getMessage());
    assertTrue(error.getMessage().contains("@eventHeader"), error.getMessage());
    assertTrue(error.getMessage().contains("ChatMessage$id"), error.getMessage());
  }

  @Test
  void eventPayloadIsRejected() {
    String model =
        CBOR_MODEL.replace(
            "structure ChatMessage { @required text: String }",
            "structure ChatMessage { @eventPayload text: String }");
    var error =
        assertThrows(
            CodegenException.class,
            () -> PluginTestHarness.generate(model, "test.stream#Svc", "test::stream"));
    assertTrue(error.getMessage().contains("cpp-codegen"), error.getMessage());
    assertTrue(error.getMessage().contains("@eventPayload"), error.getMessage());
    assertTrue(error.getMessage().contains("ChatMessage$text"), error.getMessage());
  }

  @Test
  void initialRequestMembersAreRejectedWhereTheUpgradeCannotCarryThem() {
    // The one assemblable shape of a body-bound initial-request member:
    // rpcv2Cbor, whose fixed upgrade URI binds nothing. Under simpleRestJson
    // the equivalent model cannot even assemble — Smithy requires
    // @httpPayload on the stream member and then rejects unbound siblings —
    // so the generator's REST-side check is a defensive invariant, not a
    // testable surface.
    String model =
        CBOR_MODEL.replace(
            "input := { events: ClientEvents }", "input := { room: String, events: ClientEvents }");
    var error =
        assertThrows(
            CodegenException.class,
            () -> PluginTestHarness.generate(model, "test.stream#Svc", "test::stream"));
    assertTrue(error.getMessage().contains("cpp-codegen"), error.getMessage());
    assertTrue(error.getMessage().contains("initial-request member 'room'"), error.getMessage());
    assertTrue(error.getMessage().contains("test.stream#Chat"), error.getMessage());
  }

  @Test
  void initialResponseMembersAreRejectedAsADocumentedDeferral() {
    String model =
        CBOR_MODEL.replace(
            "output := { events: ServerEvents }",
            "output := { greeting: String, events: ServerEvents }");
    var error =
        assertThrows(
            CodegenException.class,
            () -> PluginTestHarness.generate(model, "test.stream#Svc", "test::stream"));
    assertTrue(error.getMessage().contains("cpp-codegen"), error.getMessage());
    assertTrue(
        error.getMessage().contains("initial-response member 'greeting'"), error.getMessage());
    assertTrue(error.getMessage().contains("test.stream#Chat"), error.getMessage());
  }

  @Test
  void buildDepsGrowOnlyForStreamingServices() {
    String build =
        PluginTestHarness.generate(
                REST_MODEL,
                "test.stream#Svc",
                "test::stream",
                b -> b.withMember("runtimeTarget", "//runtime:core"))
            .expectFileString("/BUILD.bazel");
    // Client: the typed sessions plus the default Beast dialer. Server: the
    // WebSocketRouter lives in :server — no Beast.
    assertTrue(build.contains("\"//runtime:eventstream\""), build);
    int clientTarget = build.indexOf("name = \"client\"");
    int serverTarget = build.indexOf("name = \"server\"");
    String clientDeps = build.substring(clientTarget, serverTarget);
    String serverDeps = build.substring(serverTarget);
    assertTrue(clientDeps.contains("\"//runtime:http_beast\""), build);
    assertTrue(clientDeps.contains("\"//runtime:eventstream\""), build);
    assertTrue(serverDeps.contains("\"//runtime:eventstream\""), build);
    assertFalse(serverDeps.contains("\"//runtime:http_beast\""), build);

    // Unary services are untouched: dep-light consumers pay nothing.
    String unary =
        """
        $version: "2.0"
        namespace test.stream
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Ping] }

        @http(method: "POST", uri: "/ping")
        operation Ping { input := { name: String } }
        """;
    String unaryBuild =
        PluginTestHarness.generate(
                unary,
                "test.stream#Svc",
                "test::stream",
                b -> b.withMember("runtimeTarget", "//runtime:core"))
            .expectFileString("/BUILD.bazel");
    assertFalse(unaryBuild.contains(":eventstream"), unaryBuild);
    assertFalse(unaryBuild.contains(":http_beast"), unaryBuild);
  }

  /** REST_MODEL plus a unary neighbor, generated with the test suites on. */
  private static MockManifest restWithUnaryNeighborAndTests() {
    String model =
        REST_MODEL.replace(
                "service Svc { version: \"1\", operations: [Converse, Watch] }",
                "service Svc { version: \"1\", operations: [Converse, Watch, Ping] }")
            + """

            @http(method: "POST", uri: "/ping")
            operation Ping { input := { name: String } }
            """;
    return PluginTestHarness.generate(
        model,
        "test.stream#Svc",
        "test::stream",
        b ->
            b.withMember("runtimeTarget", "//runtime:core")
                .withMember("testsPackage", "//generated")
                .withMember("integrationTests", true));
  }

  @Test
  void smokeTestsSkipStreamingOperations() {
    String smoke = restWithUnaryNeighborAndTests().expectFileString("/tests/smoke_test.cc");
    // The unary neighbor round-trips; the streaming operations get no
    // unary-shaped test (no minimal output, no round-trip TEST).
    assertTrue(smoke.contains("TEST(SvcSmokeTest, PingRoundTrips)"), smoke);
    assertFalse(smoke.contains("TEST(SvcSmokeTest, ConverseRoundTrips)"), smoke);
    assertFalse(smoke.contains("TEST(SvcSmokeTest, WatchRoundTrips)"), smoke);
    assertFalse(smoke.contains("MinimalConverseOutput"), smoke);
    // The handler subclass still implements the streaming interface, via the
    // close-immediately stub (spelled with the header's session alias).
    assertTrue(smoke.contains("ConverseServerStream& stream"), smoke);
    assertTrue(smoke.contains("stream.Close();"), smoke);
  }

  @Test
  void integrationTestsSkipStreamingOperations() {
    String tests = restWithUnaryNeighborAndTests().expectFileString("/tests/integration_test.cc");
    assertTrue(tests.contains("PingRandomRoundTrips"), tests);
    assertFalse(tests.contains("ConverseRandomRoundTrips"), tests);
    assertFalse(tests.contains("WatchRandomRoundTrips"), tests);
    // No scripted state for streaming operations, and no error test for the
    // streaming-only RoomGone; the scripted handler keeps the interface
    // implemented through the stub.
    assertFalse(tests.contains("lastConverse"), tests);
    assertFalse(tests.contains("RoomGoneMapsAcrossTheWire"), tests);
    assertTrue(tests.contains("ConverseServerStream& stream"), tests);
  }

  @Test
  void unaryServicesEmitNoStreamingSurface() {
    String model =
        """
        $version: "2.0"
        namespace test.stream
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Ping] }

        @http(method: "POST", uri: "/ping")
        operation Ping { input := { name: String } }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.stream#Svc", "test::stream");
    String client = manifest.expectFileString("/include/test/stream/client.h");
    assertFalse(client.contains("event_stream.h"), client);
    String clientSource = manifest.expectFileString("/src/client.cc");
    assertFalse(clientSource.contains("DialStream"), clientSource);
    String server = manifest.expectFileString("/include/test/stream/server.h");
    assertFalse(server.contains("StreamRouter"), server);
    assertFalse(server.contains("websocket_router.h"), server);
  }
}
