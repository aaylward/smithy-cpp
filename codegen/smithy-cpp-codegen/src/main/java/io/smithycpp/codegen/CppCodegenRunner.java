package io.smithycpp.codegen;

import java.nio.file.Paths;
import software.amazon.smithy.build.FileManifest;
import software.amazon.smithy.build.PluginContext;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.node.Node;
import software.amazon.smithy.model.node.ObjectNode;

/**
 * Command-line entry point used by the {@code generateFixtures} Gradle task (and anyone who wants
 * generation without smithy-build):
 *
 * <pre>
 * CppCodegenRunner --model M.smithy --service NS#Svc --namespace a::b \
 *     --runtime-target //runtime:core --output DIR
 * </pre>
 */
public final class CppCodegenRunner {

  private CppCodegenRunner() {}

  public static void main(String[] args) {
    String modelPath = null;
    String service = null;
    String namespace = null;
    String runtimeTarget = "@smithy_cpp//runtime:core";
    String output = null;
    for (int i = 0; i + 1 < args.length; i += 2) {
      switch (args[i]) {
        case "--model" -> modelPath = args[i + 1];
        case "--service" -> service = args[i + 1];
        case "--namespace" -> namespace = args[i + 1];
        case "--runtime-target" -> runtimeTarget = args[i + 1];
        case "--output" -> output = args[i + 1];
        default -> throw new IllegalArgumentException("unknown argument: " + args[i]);
      }
    }
    if (modelPath == null || service == null || namespace == null || output == null) {
      throw new IllegalArgumentException(
          "required: --model <file> --service <id> --namespace <ns> --output <dir>");
    }

    Model model =
        Model.assembler()
            .discoverModels(CppCodegenRunner.class.getClassLoader())
            .addImport(Paths.get(modelPath))
            .assemble()
            .unwrap();

    ObjectNode settings =
        Node.objectNodeBuilder()
            .withMember("service", service)
            .withMember("namespace", namespace)
            .withMember("runtimeTarget", runtimeTarget)
            .build();

    PluginContext context =
        PluginContext.builder()
            .fileManifest(FileManifest.create(Paths.get(output)))
            .model(model)
            .settings(settings)
            .build();
    new CppCodegenPlugin().execute(context);
  }
}
