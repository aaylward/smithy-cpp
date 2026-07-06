package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;

/**
 * Protocol plug point (mirrors smithy-rs's protocol layer): owns the HTTP binding/dispatch code
 * emitted into generated clients, delegating document serde to the shared serde functions.
 */
interface ProtocolGenerator {

  /** e.g. "restJson1". */
  String name();

  /** Value for the Accept header and error-body content type. */
  String contentType();

  /** Extra runtime Bazel targets the generated client needs (":json" and/or ":cbor"). */
  List<String> runtimeDeps();

  /** Includes needed by client.cc beyond the shared set. */
  List<String> clientIncludes();

  /** Emits the file-local helpers (error deserializer etc.) into client.cc's anon namespace. */
  void writeClientHelpers(CppWriter w, CppContext context);

  /** Emits the body of one operation method (inside the function braces). */
  void writeOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation);
}
