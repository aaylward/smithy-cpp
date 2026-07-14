package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;

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

  private static final Pattern BANNED_TYPES =
      Pattern.compile("throw\\s+new\\s+(IllegalArgumentException|UnsupportedOperationException)");

  private static Stream<Path> generatorSources() throws IOException {
    try (Stream<Path> files = Files.list(SOURCES)) {
      return files.filter(p -> p.toString().endsWith(".java")).sorted().toList().stream();
    }
  }

  @Test
  void everyCodegenExceptionMessageCarriesTheAttributionPrefix() throws IOException {
    List<String> violations = new ArrayList<>();
    for (Path source : generatorSources().toList()) {
      String text = Files.readString(source);
      Matcher matcher = CODEGEN_THROW.matcher(text);
      while (matcher.find()) {
        String literal = matcher.group(1);
        if (!literal.startsWith("cpp-codegen: ")) {
          violations.add(source.getFileName() + ": \"" + literal + "\"");
        }
      }
    }
    assertEquals(
        List.of(),
        violations,
        "CodegenException messages must start with \"cpp-codegen: \" (and, when a shape is at"
            + " fault, name the shape and the fix)");
  }

  @Test
  void generatorDiagnosticsUseCodegenExceptionNotGenericRuntimeTypes() throws IOException {
    List<String> violations = new ArrayList<>();
    for (Path source : generatorSources().toList()) {
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
