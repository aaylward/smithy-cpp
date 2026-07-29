package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.function.Consumer;
import software.amazon.smithy.build.MockManifest;
import software.amazon.smithy.build.PluginContext;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.node.Node;
import software.amazon.smithy.model.node.ObjectNode;

/**
 * The one scaffold behind every plugin-running generator test (issue #64): assemble a model from
 * inline text, run {@link CppCodegenPlugin} against a {@link MockManifest}, and hand the manifest
 * back for assertions. Tests supply only what varies — the model text, the service id, the C++
 * namespace, and any extra settings members.
 */
final class PluginTestHarness {

  private PluginTestHarness() {}

  /** Assembles inline model text with the classpath trait models discovered. */
  static Model assembleModel(String modelText) {
    return Model.assembler()
        .discoverModels(PluginTestHarness.class.getClassLoader())
        .addUnparsedModel("test-model.smithy", modelText)
        .assemble()
        .unwrap();
  }

  /** Assembles {@code modelText} and runs the plugin with only the required settings. */
  static MockManifest generate(String modelText, String service, String namespace) {
    return generate(modelText, service, namespace, settings -> {});
  }

  /**
   * Asserts generation rejects the model with a diagnostic carrying every {@code fragment} — the
   * shared shape of every scoping-rejection test.
   */
  static void assertRejected(
      String modelText, String service, String namespace, String... fragments) {
    CodegenException error =
        assertThrows(CodegenException.class, () -> generate(modelText, service, namespace));
    for (String fragment : fragments) {
      assertTrue(error.getMessage().contains(fragment), error.getMessage());
    }
  }

  /**
   * Assembles {@code modelText} and runs the plugin, letting {@code extraSettings} add members
   * (mode, runtimeTarget, emitBuildFile, ...) to the settings object.
   */
  static MockManifest generate(
      String modelText,
      String service,
      String namespace,
      Consumer<ObjectNode.Builder> extraSettings) {
    return execute(assembleModel(modelText), service, namespace, extraSettings);
  }

  /** Runs the plugin against an already-assembled model and returns the manifest. */
  static MockManifest execute(
      Model model, String service, String namespace, Consumer<ObjectNode.Builder> extraSettings) {
    ObjectNode.Builder settings =
        Node.objectNodeBuilder().withMember("service", service).withMember("namespace", namespace);
    extraSettings.accept(settings);
    MockManifest manifest = new MockManifest();
    new CppCodegenPlugin()
        .execute(
            PluginContext.builder()
                .fileManifest(manifest)
                .model(model)
                .settings(settings.build())
                .build());
    return manifest;
  }
}
