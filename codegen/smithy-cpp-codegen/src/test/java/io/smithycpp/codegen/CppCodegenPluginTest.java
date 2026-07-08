package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
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
  void generatesTheJsonRpc2ProtocolFromItsTrait() {
    Path model =
        Paths.get(
            System.getProperty("smithycpp.repoRoot"), "examples/jsonrpc2/model/calculator.smithy");
    MockManifest manifest = new MockManifest();
    PluginContext context =
        PluginContext.builder()
            .fileManifest(manifest)
            .model(
                Model.assembler()
                    .discoverModels(CppCodegenPluginTest.class.getClassLoader())
                    .addImport(model)
                    .assemble()
                    .unwrap())
            .settings(
                Node.objectNodeBuilder()
                    .withMember("service", "example.calculator#Calculator")
                    .withMember("namespace", "example::calculator")
                    .withMember("runtimeTarget", "//runtime:core")
                    .build())
            .build();
    new CppCodegenPlugin().execute(context);

    String client = manifest.expectFileString("/src/client.cc");
    assertTrue(client.contains("envelope.emplace(\"jsonrpc\", smithy::Document(\"2.0\"));"));
    assertTrue(client.contains("envelope.emplace(\"method\", smithy::Document(\"Add\"));"));
    assertTrue(client.contains("request.target = path_prefix_ + \"/\";"));

    // One route for the whole service, dispatching on the envelope's method.
    String server = manifest.expectFileString("/src/server.cc");
    assertEquals(1, server.split("router_->Add\\(", -1).length - 1);
    assertTrue(server.contains("if (method_name == \"Divide\")"));
    assertTrue(server.contains("return JsonRpcError(-32601, \"UnknownOperationException\""));
    // Modeled errors: @httpError status as the code, fq shape id in __type.
    assertTrue(
        server.contains(
            "return JsonRpcError(422, \"example.calculator#DivisionByZero\", \"\","
                + " std::move(body), id);"));
  }

  @Test
  void generatesRecursiveShapes() {
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
                    label: Wrapper
                    children: TreeNodes
                }
                structure Wrapper {
                    node: TreeNode
                }
                list TreeNodes { member: TreeNode }
                """)
            .assemble()
            .unwrap();
    MockManifest manifest = new MockManifest();
    PluginContext context =
        PluginContext.builder()
            .fileManifest(manifest)
            .model(model)
            .settings(
                Node.objectNodeBuilder()
                    .withMember("service", "test.rec#Svc")
                    .withMember("namespace", "test::rec")
                    .build())
            .build();
    new CppCodegenPlugin().execute(context);
    String header = manifest.expectFileString("/include/test/rec/types.h");
    // Structure-to-structure cycle edges box; list cycles ride std::vector's
    // incomplete-element support behind a forward declaration.
    assertTrue(header.contains("std::optional<smithy::Boxed<TreeNode>> node{};"), header);
    assertTrue(header.contains("std::optional<std::vector<TreeNode>> children{};"), header);
    assertTrue(header.contains("struct TreeNode;"), header);
  }

  @Test
  void rejectsBacktrackingOnlyPatterns() {
    // Backreferences and lookaround need a backtracking engine; the
    // linear-time ReDoS-safe matcher refuses them at generation time.
    Model model =
        Model.assembler()
            .discoverModels(CppCodegenPluginTest.class.getClassLoader())
            .addUnparsedModel(
                "backref.smithy",
                """
                $version: "2.0"
                namespace test.redos
                use smithy.cpp.protocols#jsonRpc2

                @jsonRpc2
                service Svc { version: "1", operations: [Op] }
                operation Op {
                    input := {
                        @pattern("^(a+)\\\\1$")
                        doubled: String
                    }
                }
                """)
            .assemble()
            .unwrap();
    PluginContext context =
        PluginContext.builder()
            .fileManifest(new MockManifest())
            .model(model)
            .settings(
                Node.objectNodeBuilder()
                    .withMember("service", "test.redos#Svc")
                    .withMember("namespace", "test::redos")
                    .build())
            .build();
    CodegenException error =
        assertThrows(CodegenException.class, () -> new CppCodegenPlugin().execute(context));
    assertTrue(error.getMessage().contains("backreference"));
    assertTrue(error.getMessage().contains("^(a+)\\1$"));
  }

  @Test
  void rejectsRecursionThroughUnionMembers() {
    Model model =
        Model.assembler()
            .addUnparsedModel(
                "recursive-union.smithy",
                """
                $version: "2.0"
                namespace test.rec

                service Svc { version: "1", operations: [Op] }
                operation Op { input := { tree: TreeNode } }

                structure TreeNode {
                    value: TreeValue
                }
                union TreeValue {
                    leaf: String
                    node: TreeNode
                }
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
    assertTrue(error.getMessage().contains("union member"));
    assertTrue(error.getMessage().contains("TreeValue"));
  }

  // A jsonRpc2 server generated from an inline model; returns the manifest so a
  // test can assert on the emitted C++.
  private static MockManifest generateJsonRpc2(String filename, String modelText, String service) {
    Model model =
        Model.assembler()
            .discoverModels(CppCodegenPluginTest.class.getClassLoader())
            .addUnparsedModel(filename, modelText)
            .assemble()
            .unwrap();
    MockManifest manifest = new MockManifest();
    new CppCodegenPlugin()
        .execute(
            PluginContext.builder()
                .fileManifest(manifest)
                .model(model)
                .settings(
                    Node.objectNodeBuilder()
                        .withMember("service", service)
                        .withMember("namespace", "test::gen")
                        .build())
                .build());
    return manifest;
  }

  private static CodegenException assertJsonRpc2Rejected(
      String filename, String modelText, String service) {
    Model model =
        Model.assembler()
            .discoverModels(CppCodegenPluginTest.class.getClassLoader())
            .addUnparsedModel(filename, modelText)
            .assemble()
            .unwrap();
    PluginContext context =
        PluginContext.builder()
            .fileManifest(new MockManifest())
            .model(model)
            .settings(
                Node.objectNodeBuilder()
                    .withMember("service", service)
                    .withMember("namespace", "test::gen")
                    .build())
            .build();
    return assertThrows(CodegenException.class, () -> new CppCodegenPlugin().execute(context));
  }

  @Test
  void escapesEnumValueSetInTheValidationMessage() {
    // An enum wire value containing a quote/backslash must be escaped in the
    // generated value-set message, not emitted raw (which would not compile).
    MockManifest manifest =
        generateJsonRpc2(
            "enum-escape.smithy",
            """
            $version: "2.0"
            namespace test.gen
            use smithy.cpp.protocols#jsonRpc2

            @jsonRpc2
            service Svc { version: "1", operations: [Op] }
            operation Op { input := { grade: Grade } }
            enum Grade {
                TRICKY = "a\\"b\\\\c"
                PLAIN = "plain"
            }
            """,
            "test.gen#Svc");
    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(server.contains("enum value set:"));
    // The quote and backslash are escaped; the raw form never appears.
    assertTrue(server.contains("a\\\"b\\\\c"));
    assertFalse(server.contains("[a\"b"));
  }

  @Test
  void rejectsPatternContainingTheRawStringDelimiter() {
    // A valid regex (balanced group) that nonetheless contains the raw-string
    // closing sequence )__smithy" — emitting it verbatim would break the literal.
    CodegenException error =
        assertJsonRpc2Rejected(
            "delim.smithy",
            """
            $version: "2.0"
            namespace test.gen
            use smithy.cpp.protocols#jsonRpc2

            @jsonRpc2
            service Svc { version: "1", operations: [Op] }
            operation Op { input := { @pattern("(a)__smithy\\".*") s: String } }
            """,
            "test.gen#Svc");
    assertTrue(error.getMessage().contains("raw-string delimiter"));
  }

  @Test
  void rejectsEnumMemberNameCollision() {
    CodegenException error =
        assertJsonRpc2Rejected(
            "enum-collide.smithy",
            """
            $version: "2.0"
            namespace test.gen
            use smithy.cpp.protocols#jsonRpc2

            @jsonRpc2
            service Svc { version: "1", operations: [Op] }
            operation Op { input := { e: E } }
            enum E {
                foo_bar = "1"
                foo__bar = "2"
            }
            """,
            "test.gen#Svc");
    assertTrue(error.getMessage().contains("generated name"));
    assertTrue(error.getMessage().contains("foo_bar"));
    assertTrue(error.getMessage().contains("foo__bar"));
  }

  @Test
  void rejectsEnumMemberCollidingWithTheUnknownSentinel() {
    CodegenException error =
        assertJsonRpc2Rejected(
            "enum-unknown.smithy",
            """
            $version: "2.0"
            namespace test.gen
            use smithy.cpp.protocols#jsonRpc2

            @jsonRpc2
            service Svc { version: "1", operations: [Op] }
            operation Op { input := { e: E } }
            enum E {
                unknown = "1"
                known = "2"
            }
            """,
            "test.gen#Svc");
    assertTrue(error.getMessage().contains("reserved generated name"));
    assertTrue(error.getMessage().contains("kUnknown"));
  }

  @Test
  void emitsInt64MinRangeBoundAndDefaultWithoutOverflow() {
    // -9223372036854775808 is not a writable C++ decimal literal; it must be
    // emitted via the INT64_MIN idiom in both the @range comparison and the
    // @default initializer.
    MockManifest manifest =
        generateJsonRpc2(
            "int64min.smithy",
            """
            $version: "2.0"
            namespace test.gen
            use smithy.cpp.protocols#jsonRpc2

            @jsonRpc2
            service Svc { version: "1", operations: [Op] }
            operation Op {
                input := {
                    @range(min: -9223372036854775808)
                    bounded: Long

                    @default(-9223372036854775808)
                    defaulted: Long
                }
            }
            """,
            "test.gen#Svc");
    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(server.contains("(-9223372036854775807LL - 1)"));
    // The comparison must not emit the bare, ill-formed decimal literal.
    assertFalse(server.contains("< -9223372036854775808"));
  }
}
