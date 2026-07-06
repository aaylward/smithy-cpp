package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ServiceLoader;
import java.util.stream.StreamSupport;
import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;
import software.amazon.smithy.build.PluginContext;
import software.amazon.smithy.build.SmithyBuildPlugin;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.node.Node;
import software.amazon.smithy.model.node.ObjectNode;

class CppCodegenPluginTest {

  private static Model weatherModel() {
    Path model =
        Paths.get(
            System.getProperty("smithycpp.repoRoot"), "examples/weather/model/weather.smithy");
    return Model.assembler()
        .discoverModels(CppCodegenPluginTest.class.getClassLoader())
        .addImport(model)
        .assemble()
        .unwrap();
  }

  private static ObjectNode weatherSettings() {
    return Node.objectNodeBuilder()
        .withMember("service", "example.weather#Weather")
        .withMember("namespace", "example::weather")
        .withMember("runtimeTarget", "//runtime:core")
        .build();
  }

  private static MockManifest generateWeather() {
    MockManifest manifest = new MockManifest();
    PluginContext context =
        PluginContext.builder()
            .fileManifest(manifest)
            .model(weatherModel())
            .settings(weatherSettings())
            .build();
    new CppCodegenPlugin().execute(context);
    return manifest;
  }

  @Test
  void reservesThePluginName() {
    assertEquals("cpp-codegen", new CppCodegenPlugin().getName());
  }

  @Test
  void isDiscoverableViaServiceLoader() {
    boolean found =
        StreamSupport.stream(ServiceLoader.load(SmithyBuildPlugin.class).spliterator(), false)
            .anyMatch(plugin -> plugin instanceof CppCodegenPlugin);
    assertTrue(found, "CppCodegenPlugin must be registered in META-INF/services");
  }

  @Test
  void generatesTypesHeaderAndBuildFile() {
    MockManifest manifest = generateWeather();
    String header = manifest.expectFileString("/include/example/weather/types.h");
    assertTrue(header.contains("struct GetCityOutput {"));
    assertTrue(header.contains("struct CityCoordinates {"));
    assertTrue(header.contains("struct NoSuchResource {"));
    assertTrue(header.contains("std::optional<std::string> nextToken{};"));
    assertTrue(header.contains("namespace example::weather {"));
    assertTrue(header.contains("smithy::Timestamp time{};"));
    // Topological order: CityCoordinates is declared before its user GetCityOutput.
    assertTrue(
        header.indexOf("struct CityCoordinates {") < header.indexOf("struct GetCityOutput {"));

    String build = manifest.expectFileString("/BUILD.bazel");
    assertTrue(build.contains("name = \"types\""));
    assertTrue(build.contains("//runtime:core"));
  }

  @Test
  void outputIsByteDeterministic() {
    MockManifest first = generateWeather();
    MockManifest second = generateWeather();
    assertEquals(first.getFiles(), second.getFiles());
    for (Path file : first.getFiles()) {
      assertEquals(first.expectFileString(file), second.expectFileString(file), file.toString());
    }
  }

  @Test
  void rejectsRecursiveShapes() {
    Model model =
        Model.assembler()
            .addUnparsedModel(
                "recursive.smithy",
                """
                $version: "2.0"
                namespace test.rec

                service Svc { version: "1", operations: [Op] }
                operation Op { input := { tree: TreeNode } }

                structure TreeNode {
                    children: TreeNodes
                }
                list TreeNodes { member: TreeNode }
                """)
            .assemble()
            .unwrap();
    PluginContext context =
        PluginContext.builder()
            .fileManifest(new MockManifest())
            .model(model)
            .settings(
                Node.objectNodeBuilder()
                    .withMember("service", "test.rec#Svc")
                    .withMember("namespace", "test::rec")
                    .build())
            .build();
    CodegenException error =
        assertThrows(CodegenException.class, () -> new CppCodegenPlugin().execute(context));
    assertTrue(error.getMessage().contains("recursive"));
    assertTrue(error.getMessage().contains("TreeNode"));
  }
}
