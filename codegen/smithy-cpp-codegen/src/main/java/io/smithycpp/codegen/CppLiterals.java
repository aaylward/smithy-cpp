package io.smithycpp.codegen;

import java.nio.charset.StandardCharsets;

/** C++ literal formatting for generated code (string escaping, float/double text). */
final class CppLiterals {

  private CppLiterals() {}

  /**
   * A double-quoted C++ string literal for arbitrary text. Non-ASCII and control bytes are emitted
   * as three-digit octal escapes (never ambiguous before a following digit, unlike {@code \x}).
   */
  static String stringLiteral(String text) {
    StringBuilder out = new StringBuilder("\"");
    for (byte raw : text.getBytes(StandardCharsets.UTF_8)) {
      int b = raw & 0xFF;
      switch (b) {
        case '"' -> out.append("\\\"");
        case '\\' -> out.append("\\\\");
        case '\n' -> out.append("\\n");
        case '\r' -> out.append("\\r");
        case '\t' -> out.append("\\t");
        default -> {
          if (b >= 0x20 && b < 0x7F) {
            out.append((char) b);
          } else {
            out.append(String.format("\\%03o", b));
          }
        }
      }
    }
    return out.append('"').toString();
  }

  /** Text of a double literal; guarantees a decimal point or exponent so the type stays double. */
  static String doubleLiteral(double value) {
    String text = Double.toString(value);
    return text; // Double.toString always includes '.' or 'E'.
  }

  static String floatLiteral(double value) {
    return doubleLiteral(value) + "F";
  }
}
