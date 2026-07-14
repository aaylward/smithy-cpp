package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import software.amazon.smithy.codegen.core.CodegenException;

/**
 * Pins the runner's --omit-operation contract (issue #47): omits prune unsupported operations, and
 * the list self-polices — an omit whose operation the generator NOW supports fails the run instead
 * of silently staying excluded forever ("must only shrink").
 */
class CppCodegenRunnerTest {

  // Bad's input reaches recursion through a union member, which the generator
  // rejects at the door — the archetypal "unsupported feature" omit target.
  private static final String MODEL =
      """
      $version: "2.0"
      namespace test.omit

      service Svc { version: "1", operations: [Good, Bad] }
      operation Good { input := { name: String } }
      operation Bad { input := { tree: TreeNode } }

      structure TreeNode {
          value: TreeValue
      }
      union TreeValue {
          leaf: String
          node: TreeNode
      }
      """;

  @TempDir Path tmp;

  private String[] args(Path modelFile, Path output, String... omitOperations) {
    var argv = new java.util.ArrayList<String>();
    argv.addAll(
        java.util.List.of(
            "--model",
            modelFile.toString(),
            "--service",
            "test.omit#Svc",
            "--namespace",
            "test::omit",
            "--output",
            output.toString()));
    for (String op : omitOperations) {
      argv.addAll(java.util.List.of("--omit-operation", op));
    }
    return argv.toArray(new String[0]);
  }

  private Path writeModel() throws IOException {
    Path modelFile = tmp.resolve("omit.smithy");
    Files.writeString(modelFile, MODEL);
    return modelFile;
  }

  @Test
  void validOmitPrunesTheUnsupportedOperation() throws IOException {
    Path output = tmp.resolve("out-valid");
    CppCodegenRunner.main(args(writeModel(), output, "test.omit#Bad"));
    String header = Files.readString(output.resolve("include/test/omit/types.h"));
    assertTrue(header.contains("struct GoodInput"), header);
    // The omitted operation and everything only it referenced are gone.
    assertFalse(header.contains("TreeNode"), header);
  }

  @Test
  void staleOmitOfASupportedOperationFailsTheRun() throws IOException {
    Path output = tmp.resolve("out-stale");
    // Good generates cleanly, so omitting it is stale; Bad's omit is still
    // legitimate (and keeps Good's probe from failing for unrelated reasons).
    CodegenException error =
        assertThrows(
            CodegenException.class,
            () ->
                CppCodegenRunner.main(
                    args(writeModel(), output, "test.omit#Bad", "test.omit#Good")));
    assertTrue(
        error.getMessage().contains("stale --omit-operation test.omit#Good"), error.getMessage());
    assertTrue(error.getMessage().contains("must only shrink"), error.getMessage());
  }

  @Test
  void staleProbeFailureIsAttributableWhenOtherOmitsRemain() throws IOException {
    // Bad's probe retains Bad but still omits nothing else unsupported: it
    // must fail (recursion), so Bad's omit survives — no false stale report.
    Path output = tmp.resolve("out-attributable");
    assertDoesNotThrow(() -> CppCodegenRunner.main(args(writeModel(), output, "test.omit#Bad")));
  }

  @Test
  void unknownOmitIdStillFailsFast() throws IOException {
    Path output = tmp.resolve("out-unknown");
    assertThrows(
        RuntimeException.class,
        () -> CppCodegenRunner.main(args(writeModel(), output, "test.omit#NoSuchOp")));
  }
}
