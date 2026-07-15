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

  private static final String ERRORS_MODEL =
      """
      $version: "2.0"
      namespace test.shape
      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Ping] }

      @http(method: "POST", uri: "/ping")
      operation Ping {
          input := { @required id: String }
          errors: [NotFound, Quota]
      }

      @error("client")
      @httpError(404)
      structure NotFound { message: String }

      @error("client")
      @httpError(429)
      structure Quota { message: String }
      """;

  @Test
  void operationsGrowTypedErrorListings() {
    // Issue #49: modeled-error dispatch was stringly-typed — consumers
    // compared Error::code() text and guessed the detail<T>() type. Every
    // operation with modeled errors now gets a <Op>Errors listing in
    // client.h: FromError() matches kind + code and carries the typed
    // detail, and the union accessor surface (is_/as_/or_null/case_name/
    // visit) makes dispatch exhaustive and typo-proof.
    String client =
        PluginTestHarness.generate(ERRORS_MODEL, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/client.h");
    assertTrue(client.contains("class PingErrors {"), client);
    assertTrue(
        client.contains("static PingErrors FromError(const smithy::Error& error) {"), client);
    assertTrue(
        client.contains("if (error.kind() != smithy::ErrorKind::kModeled) return result;"), client);
    assertTrue(client.contains("if (error.code() == \"NotFound\")"), client);
    assertTrue(client.contains("bool is_not_found() const"), client);
    assertTrue(
        client.contains(
            "const Quota* as_quota_or_null() const" + " { return std::get_if<2>(&value_); }"),
        client);
    assertTrue(
        client.contains(
            "static constexpr const char* kNames[] ="
                + " {\"(empty)\", \"not_found\", \"quota\"};"),
        client);
    // Error listings are matched from a smithy::Error, never hand-assembled:
    // no From<Member> factories.
    assertFalse(client.contains("FromNotFound"), client);
  }

  private static final String ORDERING_MODEL =
      """
      $version: "2.0"
      namespace test.shape
      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Ping] }

      @http(method: "POST", uri: "/ping")
      operation Ping { input := { status: Status, size: Size, empty: Empty } }

      union Status { pending: Pending, ready: Ready }
      structure Pending { position: Integer }
      structure Ready { at: Timestamp }
      structure Empty {}
      enum Size { SMALL, LARGE }
      """;

  @Test
  void generatedTypesAreOrderedAndEnumsSwitchDirectly() {
    // Issue #49: generated types offered only operator==, so they couldn't
    // key a std::map, and enums needed .value() before a switch. Structs,
    // unions, and enums now default operator<=> beside operator==, and the
    // enum wrapper converts implicitly to its Value so `switch (size)` works
    // — with explicit Value equality friends so the conversion introduces no
    // overload ambiguity.
    String types =
        PluginTestHarness.generate(ORDERING_MODEL, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("#include <compare>"), types);
    assertTrue(
        types.contains("friend auto operator<=>(const Pending&, const Pending&) = default;"),
        types);
    assertTrue(
        types.contains("friend auto operator<=>(const Status&, const Status&) = default;"), types);
    assertTrue(
        types.contains("friend auto operator<=>(const Size&, const Size&) = default;"), types);
    assertTrue(types.contains("operator Value() const { return value_; }"), types);
    assertTrue(
        types.contains("friend bool operator==(const Size& a, Value b) { return a.value_ == b; }"),
        types);
  }

  @Test
  void generatedTypesHashForUnorderedContainers() {
    // Issue #49 follow-up: <=> unblocked ordered containers; unordered ones
    // need std::hash. A type gets std::hash exactly when it gets <=>. The
    // specializations must live at global scope, so they're emitted after the
    // namespace closes, in definition order (nested hashes before outer ones).
    var manifest = PluginTestHarness.generate(ORDERING_MODEL, "test.shape#Svc", "test::shape");
    String types = manifest.expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("#include \"smithy/core/hash.h\""), types);
    // Structs hash member-wise through smithy::HashValue (containers and
    // optionals have no std::hash of their own).
    assertTrue(types.contains("struct std::hash<test::shape::Pending> {"), types);
    assertTrue(
        types.contains("seed = smithy::HashCombine(seed, smithy::HashValue(value.position));"),
        types);
    // Member-less structs have nothing to mix — and must not name the unused
    // parameter (clang's -Wunused-parameter fires in every including TU).
    assertTrue(
        types.contains(
            "std::size_t operator()(const test::shape::Empty& /*value*/) const noexcept"
                + " { return 0; }"),
        types);
    // Enums hash their private (value, unknown-text) pair; unions hash the
    // engaged index + member — both need the friend declaration.
    assertTrue(types.contains("friend struct std::hash<Size>;"), types);
    assertTrue(types.contains("struct std::hash<test::shape::Size> {"), types);
    assertTrue(types.contains("friend struct std::hash<Status>;"), types);
    assertTrue(types.contains("struct std::hash<test::shape::Status> {"), types);
    // All specializations sit outside the namespace block.
    assertTrue(
        types.indexOf("}  // namespace test::shape") < types.indexOf("std::hash<test::shape::"),
        types);
    // Error listings share the tagged-variant shape, so they hash too.
    String client =
        PluginTestHarness.generate(ERRORS_MODEL, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/client.h");
    assertTrue(client.contains("struct std::hash<test::shape::PingErrors> {"), client);
  }

  @Test
  void nonOrderableMembersSkipTheDefaultedOrdering() {
    // clang hard-errors deducing a deep <=> around a Boxed recursion cycle,
    // and warns on any defaulted-but-deleted operator — so shapes that can't
    // order (recursion, Document members, transitively) get an equality-only
    // comment instead of a defaulted operator<=> (caught by CI on PR #84).
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Ping] }

        @http(method: "POST", uri: "/ping")
        operation Ping {
            input := { tree: Node, wrapper: Wrapper, plain: Plain }
            errors: [Opaque]
        }

        structure Node { value: Integer, next: Node }
        structure Meta { data: Document }
        structure Wrapper { meta: Meta }
        structure Plain { id: String }

        @error("client")
        @httpError(400)
        structure Opaque { data: Document }
        """;
    var manifest = PluginTestHarness.generate(model, "test.shape#Svc", "test::shape");
    String types = manifest.expectFileString("/include/test/shape/types.h");
    assertTrue(
        types.contains("friend bool operator==(const Node&, const Node&) = default;"), types);
    assertFalse(types.contains("operator<=>(const Node&"), types);
    assertFalse(types.contains("operator<=>(const Meta&"), types);
    assertFalse(types.contains("operator<=>(const Wrapper&"), types);
    assertTrue(types.contains("// Equality-only: a member type has no ordering"), types);
    // Non-orderability propagates: the input struct contains Node/Wrapper, so
    // it skips too — while the plain sibling keeps its ordering.
    assertFalse(types.contains("operator<=>(const PingInput&"), types);
    assertTrue(
        types.contains("friend auto operator<=>(const Plain&, const Plain&) = default;"), types);
    // Hashing follows ordering: non-orderable shapes get no std::hash either,
    // transitively — while the plain sibling keeps its specialization.
    assertFalse(types.contains("std::hash<test::shape::Node>"), types);
    assertFalse(types.contains("std::hash<test::shape::Wrapper>"), types);
    assertFalse(types.contains("std::hash<test::shape::PingInput>"), types);
    assertTrue(types.contains("struct std::hash<test::shape::Plain> {"), types);
    // The client's error listing skips too when a modeled error can't order.
    String client = manifest.expectFileString("/include/test/shape/client.h");
    assertFalse(client.contains("operator<=>(const PingErrors&"), client);
    assertTrue(client.contains("// Equality-only: a member type has no ordering"), client);
    assertFalse(client.contains("std::hash<test::shape::PingErrors>"), client);
  }

  @Test
  void typedErrorListingNameCollisionFailsWithContext() {
    // The listing's synthetic name <Op>Errors can collide with a modeled
    // shape; that must be an attributed cpp-codegen diagnostic, not silent
    // misgeneration. (The plural dodges the real DescribeSink /
    // DescribeSinkError fixture; this pins the guard for the plural too.)
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Ping] }

        @http(method: "POST", uri: "/ping")
        operation Ping {
            input := { @required id: String, extra: PingErrors }
            errors: [NotFound]
        }

        structure PingErrors { note: String }

        @error("client")
        @httpError(404)
        structure NotFound { message: String }
        """;
    var error =
        org.junit.jupiter.api.Assertions.assertThrows(
            software.amazon.smithy.codegen.core.CodegenException.class,
            () -> PluginTestHarness.generate(model, "test.shape#Svc", "test::shape"));
    assertTrue(error.getMessage().contains("cpp-codegen"), error.getMessage());
    assertTrue(error.getMessage().contains("PingErrors"), error.getMessage());
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
