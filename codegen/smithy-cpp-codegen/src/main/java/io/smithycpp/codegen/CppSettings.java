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

  private CppSettings(ShapeId service, String namespace, String runtimeTarget) {
    this.service = service;
    this.namespace = namespace;
    this.runtimeTarget = runtimeTarget;
  }

  public static CppSettings fromNode(ObjectNode node) {
    ShapeId service = ShapeId.from(node.expectStringMember("service").getValue());
    String namespace = node.expectStringMember("namespace").getValue();
    String runtimeTarget =
        node.getStringMemberOrDefault("runtimeTarget", "@smithy_cpp//runtime:core");
    if (!namespace.matches("[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*")) {
      throw new IllegalArgumentException(
          "cpp-codegen: 'namespace' must be a C++ namespace like a::b, got: " + namespace);
    }
    return new CppSettings(service, namespace, runtimeTarget);
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

  @Override
  public boolean equals(Object other) {
    if (!(other instanceof CppSettings that)) {
      return false;
    }
    return service.equals(that.service)
        && namespace.equals(that.namespace)
        && runtimeTarget.equals(that.runtimeTarget);
  }

  @Override
  public int hashCode() {
    return Objects.hash(service, namespace, runtimeTarget);
  }
}
