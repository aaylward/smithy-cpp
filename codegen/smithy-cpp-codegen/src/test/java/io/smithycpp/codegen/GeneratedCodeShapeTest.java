package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

/**
 * Exactly-once / absence pins for "generator emitted redundant/dead code" fixes with no
 * feature-owning test class — see the convention in docs/development.md (issue #68).
 */
class GeneratedCodeShapeTest {

  private static final String NO_INPUT_MODEL_TEMPLATE =
      """
      $version: "2.0"
      namespace test.shape
      use %s

      @%s
      service Svc { version: "1", operations: [Ping] }
      operation Ping {}
      """;

  private static final String UNION_MODEL =
      """
      $version: "2.0"
      namespace test.shape
      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Ping] }

      @http(method: "POST", uri: "/ping")
      operation Ping { input := { status: Status } }

      union Status { pending: Pending, ready: Ready }
      structure Pending { position: Integer }
      structure Ready { at: Timestamp }
      """;

  @Test
  void unionAccessorsFailWithContextAndOfferSafeAlternatives() {
    // Issue #49: wrong-case as_x() must die naming the union, the requested
    // member, and the engaged member — never throw a context-free
    // std::bad_variant_access — and consumers get a pointer-returning
    // accessor, case_name(), and visit() for checked access.
    String types =
        PluginTestHarness.generate(UNION_MODEL, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("#include \"smithy/core/fatal.h\""), types);
    assertTrue(types.contains("require_is(1, \"pending\");"), types);
    assertTrue(
        types.contains(
            "smithy::internal::FatalWrongUnionAccess(\"Status\", requested," + " case_name());"),
        types);
    assertTrue(
        types.contains(
            "const Pending* as_pending_or_null() const" + " { return std::get_if<1>(&value_); }"),
        types);
    assertTrue(
        types.contains(
            "static constexpr const char* kNames[] =" + " {\"(empty)\", \"pending\", \"ready\"};"),
        types);
    assertTrue(types.contains("decltype(auto) visit(Visitor&& visitor) const"), types);
  }

  @Test
  void streamingTraitIsIgnoredAndGeneratesThePlainShape() {
    // The README's "Current limitations" states that @streaming is ignored —
    // a streaming blob payload generates as an ordinary, fully buffered
    // smithy::Blob. This pins that claim (and will fail when Phase 8 makes
    // streaming real, forcing the doc sites to be updated in step).
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Upload] }

        @http(method: "POST", uri: "/upload")
        operation Upload {
            input := {
                @httpPayload
                @required
                body: StreamingBlob
            }
            output := { @required etag: String }
        }

        @streaming
        blob StreamingBlob
        """;
    String types =
        PluginTestHarness.generate(model, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("smithy::Blob body{};"), types);
  }

  @Test
  void noInputRpcv2CborRouteDecodesNoBody() {
    // #67 removed the dead body-decode from no-input server routes (clients
    // never send one; the decode could only 400 conforming empty bodies with
    // stray content). With only a no-input operation, the server must contain
    // no CBOR decode at all — its one route goes straight to the handler.
    String server =
        PluginTestHarness.generate(
                NO_INPUT_MODEL_TEMPLATE.formatted("smithy.protocols#rpcv2Cbor", "rpcv2Cbor"),
                "test.shape#Svc",
                "test::shape")
            .expectFileString("/src/server.cc");
    assertTrue(server.contains("/service/Svc/operation/Ping"), server);
    assertFalse(server.contains("cbor::Decode"), server);
  }

  @Test
  void noInputJsonRpc2DispatchNeverDeserializesParams() {
    // Same #67 fix on the jsonRpc2 side: the no-input Handle<Op> ignores the
    // params member instead of deserializing it into the empty input.
    String server =
        PluginTestHarness.generate(
                NO_INPUT_MODEL_TEMPLATE.formatted("smithy.cpp.protocols#jsonRpc2", "jsonRpc2"),
                "test.shape#Svc",
                "test::shape")
            .expectFileString("/src/server.cc");
    assertTrue(server.contains("(void)params;"), server);
    assertFalse(server.contains("DeserializePingInput"), server);
  }
}
