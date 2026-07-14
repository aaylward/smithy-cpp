package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import org.junit.jupiter.api.Test;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.ShapeType;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.RequestCompressionTrait;
import software.amazon.smithy.model.traits.synthetic.OriginalShapeIdTrait;

class ProtocolSupportTest {

  @Test
  void gzipCompressedRequiresTheGzipEncoding() {
    OperationShape plain = OperationShape.builder().id("test#Plain").build();
    assertFalse(ProtocolSupport.gzipCompressed(plain));
    OperationShape gzip =
        OperationShape.builder()
            .id("test#Gzip")
            .addTrait(RequestCompressionTrait.builder().encodings(List.of("gzip")).build())
            .build();
    assertTrue(ProtocolSupport.gzipCompressed(gzip));
    OperationShape other =
        OperationShape.builder()
            .id("test#Other")
            .addTrait(RequestCompressionTrait.builder().encodings(List.of("br")).build())
            .build();
    assertFalse(ProtocolSupport.gzipCompressed(other));
  }

  @Test
  void noModeledInputSeesUnitDirectlyAndThroughTheSyntheticShape() {
    StructureShape unit = StructureShape.builder().id("smithy.api#Unit").build();
    assertTrue(ProtocolSupport.noModeledInput(unit));
    // The dedicated-input transform replaces Unit with a synthetic <Op>Input
    // carrying OriginalShapeIdTrait; the predicate must see through it.
    StructureShape synthetic =
        StructureShape.builder()
            .id("test#OpInput")
            .addTrait(new OriginalShapeIdTrait(ShapeId.from("smithy.api#Unit")))
            .build();
    assertTrue(ProtocolSupport.noModeledInput(synthetic));
    StructureShape modeled = StructureShape.builder().id("test#RealInput").build();
    assertFalse(ProtocolSupport.noModeledInput(modeled));
    // A renamed shape that did NOT come from Unit stays modeled input.
    StructureShape renamed =
        StructureShape.builder()
            .id("test#Renamed")
            .addTrait(new OriginalShapeIdTrait(ShapeId.from("test#Original")))
            .build();
    assertFalse(ProtocolSupport.noModeledInput(renamed));
  }

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
