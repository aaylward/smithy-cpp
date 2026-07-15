package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import org.junit.jupiter.api.Test;

/**
 * The production guide's "Serving lifecycle" walkthrough must be the main() the lifecycle test
 * actually runs (same posture as {@link QuickstartMirrorTest}: the doc-taught code is the
 * CI-exercised code, by construction). Source of truth is examples/simplerestjson/serve_main.cc's
 * marker-delimited main(); this test fails whenever docs/production-guide.md stops carrying its
 * exact bytes.
 */
class ProductionGuideMirrorTest {

  private static final Path ROOT = Paths.get(System.getProperty("smithycpp.repoRoot"));

  @Test
  void servingLifecycleSnippetIsTheCompiledExampleMain() throws IOException {
    String example = Files.readString(ROOT.resolve("examples/simplerestjson/serve_main.cc"));
    int begin = example.indexOf("// [production-guide:main]");
    int end = example.indexOf("// [/production-guide:main]");
    assertTrue(begin >= 0 && end > begin, "markers missing from serve_main.cc");
    String block = example.substring(example.indexOf("\nint main", begin) + 1, end).strip();
    assertTrue(
        Files.readString(ROOT.resolve("docs/production-guide.md")).contains(block),
        "docs/production-guide.md's serving-lifecycle snippet no longer matches"
            + " examples/simplerestjson/serve_main.cc's main() verbatim; update whichever side"
            + " changed (the example is the source of truth):\n"
            + block);
  }
}
