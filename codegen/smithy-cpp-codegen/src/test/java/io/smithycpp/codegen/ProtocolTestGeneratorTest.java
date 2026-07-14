package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.codegen.core.WriterDelegator;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.node.Node;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * Pins the exclusion-list contract (issue #47): entries self-police (unused entries fail
 * generation), but staleness must not be coupled to which generation paths a task enables, and
 * cross-protocol test cases must mark their entries live even though they never generate here.
 */
class ProtocolTestGeneratorTest {

  // Ping carries one case for this protocol and one pinned to rpcv2Cbor —
  // the cross-protocol shape future multi-protocol suites will have.
  private static final String MODEL =
      """
      $version: "2.0"
      namespace test.excl

      use alloy#simpleRestJson
      use smithy.test#httpRequestTests

      @simpleRestJson
      service Svc { version: "1", operations: [Ping] }

      @http(method: "POST", uri: "/ping")
      @httpRequestTests([
          {
              id: "PingBasic"
              protocol: simpleRestJson
              method: "POST"
              uri: "/ping"
              params: {}
          }
          {
              id: "PingCbor"
              protocol: smithy.protocols#rpcv2Cbor
              method: "POST"
              uri: "/service/Svc/operation/Ping"
              params: {}
          }
      ])
      operation Ping { input := {} }
      """;

  private record Harness(
      ProtocolTestGenerator generator, CppContext context, MockManifest manifest) {}

  private static Harness harness(
      boolean standardTests, boolean malformedTests, Map<String, String> exclusions) {
    Model model = PluginTestHarness.assembleModel(MODEL);
    CppSettings settings =
        CppSettings.fromNode(
            Node.objectNodeBuilder()
                .withMember("service", "test.excl#Svc")
                .withMember("namespace", "test::excl")
                .build());
    CppSymbolProvider symbols = new CppSymbolProvider(model, settings);
    MockManifest manifest = new MockManifest();
    CppContext context =
        new CppContext(
            model,
            settings,
            symbols,
            manifest,
            new WriterDelegator<>(manifest, symbols, new CppWriter.Factory(settings)),
            List.of(),
            symbols);
    ServiceShape service = model.expectShape(ShapeId.from("test.excl#Svc"), ServiceShape.class);
    OperationShape ping = model.expectShape(ShapeId.from("test.excl#Ping"), OperationShape.class);
    ProtocolTestGenerator generator =
        new ProtocolTestGenerator(
            context,
            service,
            HttpJsonBindingProtocol.simpleRestJson(),
            List.of(ping),
            standardTests,
            malformedTests,
            exclusions);
    return new Harness(generator, context, manifest);
  }

  private static void flush(Harness h) {
    h.generator().run();
    // Mirror the framework's end-of-run flush so files land in the manifest.
    h.context().writerDelegator().flushWriters();
  }

  @Test
  void matchedExclusionSuppressesTheTestAndDoesNotThrow() {
    Harness h = harness(true, false, Map.of("request PingBasic", "not implemented yet"));
    assertDoesNotThrow(() -> flush(h));
  }

  @Test
  void unmatchedEntryOnAnExecutedPathThrows() {
    Harness h = harness(true, false, Map.of("request NoSuchTest", "stale reason"));
    CodegenException error = assertThrows(CodegenException.class, () -> flush(h));
    assertTrue(error.getMessage().contains("request NoSuchTest"), error.getMessage());
  }

  @Test
  void serverMalformedEntryIsNotJudgedWhenMalformedTestsAreOff() {
    // The #47 trap: with malformedTests=false this entry could never be
    // marked used, so generation threw for a config that is simply narrower.
    Harness h = harness(true, false, Map.of("server-malformed SomeCase", "pending"));
    assertDoesNotThrow(() -> flush(h));
  }

  @Test
  void serverMalformedEntryIsJudgedWhenItsPathRuns() {
    Harness h = harness(true, true, Map.of("server-malformed NoSuchCase", "stale reason"));
    CodegenException error = assertThrows(CodegenException.class, () -> flush(h));
    assertTrue(error.getMessage().contains("server-malformed NoSuchCase"), error.getMessage());
  }

  @Test
  void anyEntryIsOnlyJudgedWhenEveryPathRan() {
    // "any" can match in either path, so a partial run cannot prove it stale.
    assertDoesNotThrow(() -> flush(harness(true, false, Map.of("any NoSuchTest", "pending"))));
    CodegenException error =
        assertThrows(
            CodegenException.class,
            () -> flush(harness(true, true, Map.of("any NoSuchTest", "stale reason"))));
    assertTrue(error.getMessage().contains("any NoSuchTest"), error.getMessage());
  }

  @Test
  void crossProtocolCaseMarksItsEntryLiveWithoutGeneratingOrCommenting() {
    // PingCbor belongs to rpcv2Cbor: it never generates in this suite, but an
    // entry naming it must not read as stale (the pre-fix short-circuit
    // skipped exclusion accounting entirely for cross-protocol cases).
    Harness h = harness(true, false, Map.of("request PingCbor", "tracked upstream"));
    assertDoesNotThrow(() -> flush(h));
    String tests = h.manifest().expectFileString("/tests/request_tests.cc");
    assertTrue(tests.contains("PingBasic"), tests);
    assertFalse(tests.contains("PingCbor"), tests);
    // Not listed as "excluded here": the entry suppressed nothing in this suite.
    assertFalse(tests.contains("tracked upstream"), tests);
  }
}
