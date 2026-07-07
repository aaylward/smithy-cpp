package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * Protocol plug point (mirrors smithy-rs's protocol layer): owns the HTTP binding/dispatch code
 * emitted into generated clients, delegating document serde to the shared serde functions.
 */
interface ProtocolGenerator {

  /** e.g. "restJson1". */
  String name();

  /** The protocol trait's shape id (used to filter protocol test cases). */
  ShapeId traitId();

  /** Value for the Accept header and error-body content type. */
  String contentType();

  /** Extra runtime Bazel targets the generated client needs (":json" and/or ":cbor"). */
  List<String> runtimeDeps();

  /** Includes needed by client.cc beyond the shared set. */
  List<String> clientIncludes();

  /** Whether the protocol renames JSON body keys via @jsonName (restJson1 does). */
  default boolean usesJsonName() {
    return false;
  }

  /** Emits the file-local helpers (error deserializer etc.) into client.cc's anon namespace. */
  void writeClientHelpers(CppWriter w, CppContext context);

  /**
   * Emits statements patching protocol-specific bindings (e.g. restJson1 error @httpHeader members)
   * into {@code detail->...}; runs inside Make&lt;Error&gt;Error with {@code response} in scope.
   * Default: nothing.
   */
  default void writeErrorDetailPatches(
      CppWriter w, CppContext context, software.amazon.smithy.model.shapes.StructureShape error) {}

  /** Emits the body of one operation method (inside the function braces). */
  void writeOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation);

  /** Includes server.cc needs beyond the shared set. */
  List<String> serverIncludes();

  /** Emits server-side file-local helpers (error mapping, per-op parse/serialize functions). */
  void writeServerHelpers(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations);

  /** Emits the constructor statements registering one operation's route. */
  void writeServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation);
}
