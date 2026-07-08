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
    return "\"" + escapeStringBody(text) + "\"";
  }

  /**
   * Escapes arbitrary text for inclusion <em>inside</em> a double-quoted C++ string literal,
   * without the surrounding quotes — for splicing model-controlled text into a larger literal.
   * Non-ASCII and control bytes become three-digit octal escapes (never ambiguous before a
   * following digit).
   */
  static String escapeStringBody(String text) {
    StringBuilder out = new StringBuilder();
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
    return out.toString();
  }

  /**
   * A C++ literal for a 64-bit integer. {@code Long.MIN_VALUE} cannot be written as {@code
   * -9223372036854775808} — C++ parses that as negation of a value one past {@code int64_t} max,
   * which is ill-formed — so it is emitted with the header-free {@code INT64_MIN} idiom.
   */
  static String int64Literal(long value) {
    // Only the minimum needs special handling; every other int64 magnitude is a
    // valid decimal literal that promotes to a wide enough type on assignment or
    // comparison, so those are left byte-for-byte unchanged.
    return value == Long.MIN_VALUE ? "(-9223372036854775807LL - 1)" : String.valueOf(value);
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
