package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

/**
 * Exactly-once / absence pins for generator shape regressions (issue #68). Goldens ratify
 * generated-code shape rather than judge it: redundant or dead emission compiles, passes
 * conformance, and lives in the fixtures until a human reads them. Convention (development.md):
 * every fix for a "generator emitted redundant/dead code" bug lands with a pin here or beside the
 * feature's own tests (e.g. {@code
 * HttpJsonBindingProtocolTest.serverWritesResponsePrefixHeadersExactlyOnce}, pinning #63's
 * duplicate prefix-header emission) so the smell cannot quietly return.
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
