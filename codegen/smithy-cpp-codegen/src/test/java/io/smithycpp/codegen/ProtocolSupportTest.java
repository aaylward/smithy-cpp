package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.model.shapes.ShapeType;

class ProtocolSupportTest {

  @Test
  void integerBoundsMatchEachShapeWidth() {
    assertEquals("-128, 127", ProtocolSupport.int64Bounds(ShapeType.BYTE));
    assertEquals("-32768, 32767", ProtocolSupport.int64Bounds(ShapeType.SHORT));
    assertEquals("-2147483648LL, 2147483647LL", ProtocolSupport.int64Bounds(ShapeType.INTEGER));
    assertEquals("-2147483648LL, 2147483647LL", ProtocolSupport.int64Bounds(ShapeType.INT_ENUM));
  }

  @Test
  void longBoundsUseNumericLimitsNotLiterals() {
    // int64 min has no valid C++ literal spelling (issue #43); the bounds
    // for LONG must come from numeric_limits, never from decimal text.
    assertEquals(
        "std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()",
        ProtocolSupport.int64Bounds(ShapeType.LONG));
  }
}
