package io.smithycpp.codegen;

import software.amazon.smithy.build.PluginContext;
import software.amazon.smithy.build.SmithyBuildPlugin;

/**
 * Entry point for C++ code generation, registered with smithy-build as the {@code cpp-codegen}
 * plugin.
 *
 * <p>Generation itself lands in Phase 2 (see docs/PLAN.md); until then this plugin only reserves
 * the plugin name and service registration.
 */
public final class CppCodegenPlugin implements SmithyBuildPlugin {

  @Override
  public String getName() {
    return "cpp-codegen";
  }

  @Override
  public void execute(PluginContext context) {
    throw new UnsupportedOperationException(
        "C++ code generation is not implemented yet (planned for Phase 2; see docs/PLAN.md).");
  }
}
