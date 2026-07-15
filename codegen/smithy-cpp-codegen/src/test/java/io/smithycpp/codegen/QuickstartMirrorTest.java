package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import org.junit.jupiter.api.Test;

/**
 * The quickstart's documented setup must be the setup the consumer e2e actually compiles and runs
 * (issue #49: the doc taught a racy handler and an unsynced .bazelrc precisely because nothing
 * enforced the mirror). Source of truth is examples/bazel-consumer — the workspace the consumer CI
 * job builds — and this test fails whenever docs/quickstart.md stops carrying its exact bytes.
 */
class QuickstartMirrorTest {

  private static final Path ROOT = Paths.get(System.getProperty("smithycpp.repoRoot"));

  private static String read(String repoRelative) throws IOException {
    return Files.readString(ROOT.resolve(repoRelative));
  }

  @Test
  void quickstartHandlerIsTheHandlerTheConsumerE2eRuns() throws IOException {
    String test = read("examples/bazel-consumer/todo_integration_test.cc");
    int begin = test.indexOf("// [quickstart:handler]");
    int end = test.indexOf("// [/quickstart:handler]");
    assertTrue(begin >= 0 && end > begin, "markers missing from todo_integration_test.cc");
    String block = test.substring(test.indexOf("\nclass ", begin) + 1, end).strip();
    assertTrue(
        read("docs/quickstart.md").contains(block),
        "docs/quickstart.md no longer carries the consumer e2e's InMemoryHandler verbatim;"
            + " update whichever side changed (the e2e is the source of truth):\n"
            + block);
  }

  @Test
  void quickstartBazelrcIsTheConsumerE2eBazelrc() throws IOException {
    String bazelrc = read("examples/bazel-consumer/.bazelrc").strip();
    assertTrue(
        read("docs/quickstart.md").contains(bazelrc),
        "docs/quickstart.md's .bazelrc block no longer matches"
            + " examples/bazel-consumer/.bazelrc byte-for-byte:\n"
            + bazelrc);
  }

  @Test
  void consumerBazelversionMatchesTheRepoRoot() throws IOException {
    assertEquals(
        read(".bazelversion").strip(),
        read("examples/bazel-consumer/.bazelversion").strip(),
        "examples/bazel-consumer/.bazelversion drifted from the repo root's .bazelversion;"
            + " the quickstart points consumers at the example's pin");
  }

  @Test
  void quickstartModuleDepsMatchTheConsumerE2e() throws IOException {
    String quickstart = read("docs/quickstart.md");
    for (String line : read("examples/bazel-consumer/MODULE.bazel").lines().toList()) {
      if (!line.startsWith("bazel_dep(")) {
        continue;
      }
      assertTrue(
          quickstart.contains(line.strip()),
          "quickstart MODULE.bazel snippet drifted from the consumer e2e's dep: " + line);
    }
  }
}
