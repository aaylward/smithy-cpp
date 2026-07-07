package io.smithycpp.codegen;

import java.util.Objects;
import software.amazon.smithy.model.node.ObjectNode;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * Settings for the {@code cpp-codegen} smithy-build plugin.
 *
 * <pre>{@code
 * "cpp-codegen": {
 *   "service": "example.weather#Weather",
 *   "namespace": "example::weather",
 *   "runtimeTarget": "@smithy_cpp//runtime:core"
 * }
 * }</pre>
 */
public final class CppSettings {

  private final ShapeId service;
  private final String namespace;
  private final String runtimeTarget;
  private final String testsPackage;

  private CppSettings(
      ShapeId service, String namespace, String runtimeTarget, String testsPackage) {
    this.service = service;
    this.namespace = namespace;
    this.runtimeTarget = runtimeTarget;
    this.testsPackage = testsPackage;
  }

  public static CppSettings fromNode(ObjectNode node) {
    ShapeId service = ShapeId.from(node.expectStringMember("service").getValue());
    String namespace = node.expectStringMember("namespace").getValue();
    String runtimeTarget =
        node.getStringMemberOrDefault("runtimeTarget", "@smithy_cpp//runtime:core");
    String testsPackage = node.getStringMemberOrDefault("testsPackage", null);
    if (!namespace.matches("[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*")) {
      throw new IllegalArgumentException(
          "cpp-codegen: 'namespace' must be a C++ namespace like a::b, got: " + namespace);
    }
    return new CppSettings(service, namespace, runtimeTarget, testsPackage);
  }

  public ShapeId service() {
    return service;
  }

  /** C++ namespace, e.g. {@code example::weather}. */
  public String namespace() {
    return namespace;
  }

  /** Bazel label of the smithy-cpp runtime core library the generated code depends on. */
  public String runtimeTarget() {
    return runtimeTarget;
  }

  /** Include-path prefix derived from the namespace, e.g. {@code example/weather}. */
  public String includePrefix() {
    return namespace.replace("::", "/");
  }

  /** Repo-relative path of the generated types header. */
  public String typesHeaderFile() {
    return "include/" + includePrefix() + "/types.h";
  }

  public String serdeHeaderFile() {
    return "include/" + includePrefix() + "/serde.h";
  }

  public String clientHeaderFile() {
    return "include/" + includePrefix() + "/client.h";
  }

  public String serverHeaderFile() {
    return "include/" + includePrefix() + "/server.h";
  }

  /** Bazel package of the runtime, e.g. {@code //runtime} or {@code @smithy_cpp//runtime}. */
  public String runtimePackage() {
    int colon = runtimeTarget.lastIndexOf(':');
    return colon < 0 ? runtimeTarget : runtimeTarget.substring(0, colon);
  }

  /**
   * Bazel package of the generated module (e.g. {@code //examples/weather/generated}); when set,
   * tests are generated into {@code tests/}: service smoke tests always, protocol conformance tests
   * when the model carries smithy.test traits. Null otherwise.
   */
  public String testsPackage() {
    return testsPackage;
  }

  @Override
  public boolean equals(Object other) {
    if (!(other instanceof CppSettings that)) {
      return false;
    }
    return service.equals(that.service)
        && namespace.equals(that.namespace)
        && runtimeTarget.equals(that.runtimeTarget)
        && Objects.equals(testsPackage, that.testsPackage);
  }

  @Override
  public int hashCode() {
    return Objects.hash(service, namespace, runtimeTarget, testsPackage);
  }
}
