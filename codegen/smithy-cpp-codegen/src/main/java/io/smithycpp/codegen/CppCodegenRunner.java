package io.smithycpp.codegen;

import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import software.amazon.smithy.build.FileManifest;
import software.amazon.smithy.build.PluginContext;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.node.Node;
import software.amazon.smithy.model.node.ObjectNode;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.transform.ModelTransformer;

/**
 * Command-line entry point used by the {@code generateFixtures} / {@code generateProtocolTests}
 * Gradle tasks (and anyone who wants generation without smithy-build):
 *
 * <pre>
 * CppCodegenRunner [--model M.smithy]... --service NS#Svc --namespace a::b \
 *     --runtime-target //runtime:core --output DIR \
 *     [--tests-package //pkg] [--omit-operation NS#Op]... [--malformed-tests true]
 * </pre>
 *
 * <p>Models are discovered from the classpath (so the official protocol-test suites generate
 * straight from their published jars); {@code --model} adds a local file on top. {@code
 * --omit-operation} prunes operations that use features the generator doesn't support yet — the
 * generated module documents the gap, and the flag list must only shrink over time.
 */
public final class CppCodegenRunner {

  private CppCodegenRunner() {}

  public static void main(String[] args) {
    List<String> modelPaths = new ArrayList<>();
    String service = null;
    String namespace = null;
    String runtimeTarget = "@smithy_cpp//runtime:core";
    String output = null;
    String testsPackage = null;
    boolean malformedTests = false;
    boolean integrationTests = false;
    String mode = null;
    Boolean emitBuildFile = null;
    List<String> omitOperations = new ArrayList<>();
    for (int i = 0; i + 1 < args.length; i += 2) {
      switch (args[i]) {
        case "--model" -> modelPaths.add(args[i + 1]);
        case "--service" -> service = args[i + 1];
        case "--namespace" -> namespace = args[i + 1];
        case "--runtime-target" -> runtimeTarget = args[i + 1];
        case "--output" -> output = args[i + 1];
        case "--tests-package" -> testsPackage = args[i + 1];
        case "--omit-operation" -> omitOperations.add(args[i + 1]);
        case "--malformed-tests" -> malformedTests = Boolean.parseBoolean(args[i + 1]);
        case "--integration-tests" -> integrationTests = Boolean.parseBoolean(args[i + 1]);
        case "--mode" -> mode = args[i + 1];
        case "--emit-build-file" -> emitBuildFile = Boolean.parseBoolean(args[i + 1]);
        default -> throw new IllegalArgumentException("unknown argument: " + args[i]);
      }
    }
    if (service == null || namespace == null || output == null) {
      throw new IllegalArgumentException(
          "required: --service <id> --namespace <ns> --output <dir> [--model <file>]");
    }

    var assembler = Model.assembler().discoverModels(CppCodegenRunner.class.getClassLoader());
    for (String modelPath : modelPaths) {
      assembler.addImport(Paths.get(modelPath));
    }
    Model model = assembler.assemble().unwrap();

    if (!omitOperations.isEmpty()) {
      Set<Shape> toRemove = new HashSet<>();
      for (String id : omitOperations) {
        toRemove.add(model.expectShape(ShapeId.from(id)));
      }
      ModelTransformer transformer = ModelTransformer.create();
      model = transformer.removeShapes(model, toRemove);
      model = transformer.removeUnreferencedShapes(model);
    }

    ObjectNode.Builder settings =
        Node.objectNodeBuilder()
            .withMember("service", service)
            .withMember("namespace", namespace)
            .withMember("runtimeTarget", runtimeTarget);
    if (testsPackage != null) {
      settings.withMember("testsPackage", testsPackage);
    }
    if (malformedTests) {
      settings.withMember("malformedTests", true);
    }
    if (integrationTests) {
      settings.withMember("integrationTests", true);
    }
    if (mode != null) {
      settings.withMember("mode", mode);
    }
    if (emitBuildFile != null) {
      settings.withMember("emitBuildFile", emitBuildFile);
    }

    PluginContext context =
        PluginContext.builder()
            .fileManifest(FileManifest.create(Paths.get(output)))
            .model(model)
            .settings(settings.build())
            .build();
    new CppCodegenPlugin().execute(context);
  }
}
