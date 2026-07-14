package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Stream;
import org.junit.jupiter.api.Test;

/**
 * Enforces the diagnostic convention issue #47 found honored only sporadically: generator
 * diagnostics are {@code CodegenException}s whose message starts with {@code "cpp-codegen: "} (so
 * users can attribute the failure) and, where a shape is at fault, names the shape and the fix.
 * This scans the generator sources so the convention cannot silently erode — the same self-policing
 * style as the exclusion list.
 */
class DiagnosticConventionTest {

  private static final Path SOURCES =
      Paths.get(
          System.getProperty("smithycpp.repoRoot"),
          "codegen/smithy-cpp-codegen/src/main/java/io/smithycpp/codegen");

  /** `new CodegenException(` (optionally fully qualified) followed by its first string literal. */
  private static final Pattern CODEGEN_THROW =
      Pattern.compile(
          "new\\s+(?:software\\.amazon\\.smithy\\.codegen\\.core\\.)?CodegenException\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");

  /**
   * Any CodegenException construction whose first argument is not a string literal — those would
   * silently escape the prefix scan above, so the convention requires the message to start with a
   * literal.
   */
  private static final Pattern CODEGEN_THROW_NON_LITERAL =
      Pattern.compile(
          "new\\s+(?:software\\.amazon\\.smithy\\.codegen\\.core\\.)?CodegenException\\((?!\\s*\")");

  private static final Pattern BANNED_TYPES =
      Pattern.compile("throw\\s+new\\s+(IllegalArgumentException|UnsupportedOperationException)");

  private static List<Path> generatorSources() throws IOException {
    // Recursive, so a future subpackage cannot silently drop out of the audit.
    try (Stream<Path> files = Files.walk(SOURCES)) {
      return files.filter(p -> p.toString().endsWith(".java")).sorted().toList();
    }
  }

  @Test
  void everyCodegenExceptionMessageCarriesTheAttributionPrefix() throws IOException {
    List<String> violations = new ArrayList<>();
    for (Path source : generatorSources()) {
      String text = Files.readString(source);
      Matcher matcher = CODEGEN_THROW.matcher(text);
      while (matcher.find()) {
        String literal = matcher.group(1);
        if (!literal.startsWith("cpp-codegen: ")) {
          violations.add(source.getFileName() + ": \"" + literal + "\"");
        }
      }
      if (CODEGEN_THROW_NON_LITERAL.matcher(text).find()) {
        violations.add(
            source.getFileName() + ": CodegenException without a leading string literal");
      }
    }
    assertEquals(
        List.of(),
        violations,
        "CodegenException messages must start with the literal \"cpp-codegen: \" (and, when a"
            + " shape is at fault, name the shape and the fix)");
  }

  @Test
  void conventionPatternsClassifyRepresentativeConstructions() {
    // Spotless wraps long throws, putting the literal on its own line — still
    // literal-first. The inverse pattern's lookahead must own the whitespace:
    // a trailing \s* outside the lookahead backtracks and flags these.
    String wrappedLiteral = "throw new CodegenException(\n    \"cpp-codegen: x\");";
    assertTrue(CODEGEN_THROW.matcher(wrappedLiteral).find());
    assertFalse(CODEGEN_THROW_NON_LITERAL.matcher(wrappedLiteral).find());

    // The fully qualified spelling is equally in scope, and the captured
    // literal handles escaped quotes without truncating at them.
    Matcher qualified =
        CODEGEN_THROW.matcher(
            "throw new software.amazon.smithy.codegen.core.CodegenException("
                + "\"cpp-codegen: bad \\\"name\\\"\" + shape);");
    assertTrue(qualified.find());
    assertEquals("cpp-codegen: bad \\\"name\\\"", qualified.group(1));

    // Non-literal first arguments carry no scannable prefix; the inverse
    // pattern rejects them instead of letting them escape the audit.
    assertTrue(CODEGEN_THROW_NON_LITERAL.matcher("throw new CodegenException(e);").find());
    assertTrue(
        CODEGEN_THROW_NON_LITERAL
            .matcher("throw new CodegenException(String.format(\"cpp-codegen: %s\", id));")
            .find());
  }

  @Test
  void generatorDiagnosticsUseCodegenExceptionNotGenericRuntimeTypes() throws IOException {
    List<String> violations = new ArrayList<>();
    for (Path source : generatorSources()) {
      String text = Files.readString(source);
      Matcher matcher = BANNED_TYPES.matcher(text);
      while (matcher.find()) {
        violations.add(source.getFileName() + ": throw new " + matcher.group(1));
      }
    }
    assertEquals(
        List.of(),
        violations,
        "Generator diagnostics are CodegenExceptions — smithy-build reports them with codegen"
            + " context; generic runtime exceptions read as crashes");
  }
}
