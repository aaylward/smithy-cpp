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

  /**
   * Whether an unidentified error response falls back to matching the operation's declared errors
   * by HTTP status (simpleRestJson: the X-Error-Type header is the discriminator, with status-code
   * fallback when absent). Only statuses unique within the operation's error set are matched.
   */
  default boolean errorStatusFallback() {
    return false;
  }

  /**
   * Emits the file-local helpers (error deserializer etc.) into client.cc's anon namespace.
   * Per-operation helpers stay out of the serde functions' Serialize/Deserialize&lt;Shape&gt;
   * naming pattern (Parse&lt;Op&gt;Error, Build&lt;Op&gt;Response) so a shape named after an
   * operation can never hide them via C++ name hiding.
   */
  void writeClientHelpers(CppWriter w, CppContext context);

  /**
   * Emits statements patching protocol-specific bindings (e.g. simpleRestJson error @httpHeader
   * members) into the parsed error document ({@code parsed.doc}, guaranteed to be a map) before the
   * typed detail deserializes from it — so header-carried members satisfy @required. Runs inside
   * Make&lt;Error&gt;Error with {@code response} and {@code parsed} in scope. Default: nothing.
   */
  default void writeErrorDocPatches(
      CppWriter w, CppContext context, software.amazon.smithy.model.shapes.StructureShape error) {}

  /** Emits the body of one operation method (inside the function braces). */
  void writeOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation);

  /** Includes server.cc needs beyond the shared set. */
  List<String> serverIncludes();

  /** Emits server-side file-local helpers (error mapping, per-op parse/serialize functions). */
  void writeServerHelpers(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations);

  /**
   * Emits the constructor statements registering the service's routes. The default registers one
   * route per operation via {@link #writeServerRoute}; single-endpoint protocols (jsonRpc2, which
   * dispatches on the request body's {@code method} member) override this instead.
   */
  default void writeServerRoutes(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    for (OperationShape operation : operations) {
      writeServerRoute(w, context, service, operation);
    }
  }

  /** Emits the constructor statements registering one operation's route. */
  default void writeServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    throw new software.amazon.smithy.codegen.core.CodegenException(
        "cpp-codegen: "
            + name()
            + " does not emit per-operation routes; single-endpoint protocols override"
            + " writeServerRoutes instead");
  }
}
