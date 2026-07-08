package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

class CppReservedWordsTest {

  @Test
  void keywordsGetATrailingUnderscore() {
    assertEquals("class_", CppReservedWords.escape("class"));
    assertEquals("int_", CppReservedWords.escape("int"));
    assertEquals("operator_", CppReservedWords.escape("operator"));
    assertEquals("co_await_", CppReservedWords.escape("co_await"));
    assertEquals("and_", CppReservedWords.escape("and")); // alternative tokens too
  }

  @Test
  void nonKeywordsPassThroughUntouched() {
    assertEquals("value", CppReservedWords.escape("value"));
    assertEquals("classes", CppReservedWords.escape("classes")); // prefix of a keyword is fine
    assertEquals("_leading", CppReservedWords.escape("_leading"));
  }

  @Test
  void escapingIsCaseSensitiveLikeCpp() {
    // "Class" is a perfectly legal C++ identifier; only the exact keyword
    // spelling collides.
    assertEquals("Class", CppReservedWords.escape("Class"));
    assertEquals("INT", CppReservedWords.escape("INT"));
  }

  @Test
  void macroLikeNamesAreDeliberatelyNotEscaped() {
    // Documented boundary: common macro names (errno, NULL) are not keywords
    // and are left alone — generated headers avoid the system headers that
    // define them. If this ever changes, this pin is the conversation prompt.
    assertEquals("errno", CppReservedWords.escape("errno"));
    assertEquals("NULL", CppReservedWords.escape("NULL"));
  }
}
