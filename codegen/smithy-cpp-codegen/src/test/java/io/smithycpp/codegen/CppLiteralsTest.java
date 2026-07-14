package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

/**
 * CppLiterals is the chokepoint for issue #43's whole bug class: every model-controlled string that
 * reaches generated C++ goes through here. These tests pin the escaping scheme itself, so a change
 * fails loudly at the unit level instead of surfacing as a consumer's compile error.
 */
class CppLiteralsTest {

  @Test
  void plainAsciiPassesThrough() {
    assertEquals("\"hello world\"", CppLiterals.stringLiteral("hello world"));
    assertEquals("", CppLiterals.escapeStringBody(""));
  }

  @Test
  void quotesBackslashesAndWhitespaceEscape() {
    assertEquals("he said \\\"more\\\"", CppLiterals.escapeStringBody("he said \"more\""));
    assertEquals("C:\\\\temp\\\\new", CppLiterals.escapeStringBody("C:\\temp\\new"));
    assertEquals("a\\nb\\rc\\td", CppLiterals.escapeStringBody("a\nb\rc\td"));
  }

  @Test
  void controlAndNonAsciiBytesBecomeThreeDigitOctal() {
    assertEquals("\\001", CppLiterals.escapeStringBody("\u0001"));
    assertEquals("\\000", CppLiterals.escapeStringBody("\u0000"));
    assertEquals("\\177", CppLiterals.escapeStringBody("\u007F")); // DEL is not printable
    // Non-ASCII escapes per UTF-8 byte: \u00e9 is 0xC3 0xA9.
    assertEquals("\\303\\251", CppLiterals.escapeStringBody("\u00e9"));
  }

  @Test
  void octalEscapesAreUnambiguousBeforeDigits() {
    // A two-digit escape ("\1" + "1") would silently change meaning; three
    // digits terminate the escape before the literal digit.
    assertEquals("\\0011", CppLiterals.escapeStringBody("\u0001" + "1"));
  }

  @Test
  void int64MinUsesTheHeaderFreeIdiom() {
    // -9223372036854775808 is ill-formed C++ (negation of a value one past
    // int64 max); every other value stays a plain decimal literal.
    assertEquals("(-9223372036854775807LL - 1)", CppLiterals.int64Literal(Long.MIN_VALUE));
    assertEquals("9223372036854775807", CppLiterals.int64Literal(Long.MAX_VALUE));
    assertEquals("-9223372036854775807", CppLiterals.int64Literal(Long.MIN_VALUE + 1));
    assertEquals("0", CppLiterals.int64Literal(0));
    assertEquals("-1", CppLiterals.int64Literal(-1));
  }

  @Test
  void floatingLiteralsKeepTheirType() {
    // A '.' or exponent keeps the literal double instead of int.
    assertTrue(CppLiterals.doubleLiteral(1.0).contains("."));
    assertEquals("1.5", CppLiterals.doubleLiteral(1.5));
    String big = CppLiterals.doubleLiteral(1e100);
    assertTrue(big.contains(".") || big.contains("E"), big);
    assertEquals("1.5F", CppLiterals.floatLiteral(1.5));
  }

  @Test
  void nonFiniteValuesTakeTheNumericLimitsIdiomNotInvalidText() {
    // Double.toString(NaN) is "NaN" — not a C++ literal. This is the live
    // path: NodeLiteralGenerator maps string-encoded non-finite params to
    // their double values and delegates here (issue #47).
    assertEquals("std::numeric_limits<double>::quiet_NaN()", CppLiterals.doubleLiteral(Double.NaN));
    assertEquals(
        "std::numeric_limits<double>::infinity()",
        CppLiterals.doubleLiteral(Double.POSITIVE_INFINITY));
    assertEquals(
        "-std::numeric_limits<double>::infinity()",
        CppLiterals.doubleLiteral(Double.NEGATIVE_INFINITY));
    assertEquals("std::numeric_limits<float>::quiet_NaN()", CppLiterals.floatLiteral(Double.NaN));
    assertEquals(
        "std::numeric_limits<float>::infinity()",
        CppLiterals.floatLiteral(Double.POSITIVE_INFINITY));
    assertEquals(
        "-std::numeric_limits<float>::infinity()",
        CppLiterals.floatLiteral(Double.NEGATIVE_INFINITY));
  }
}
