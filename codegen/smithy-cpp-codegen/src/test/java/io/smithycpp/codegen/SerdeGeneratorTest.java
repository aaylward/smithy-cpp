package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.model.Model;

/**
 * Direct assertions on the serde SerdeGenerator/SerdeCodeGen emit for a purpose-built model —
 * previously these behaviors were pinned only transitively, by goldens and the C++ conformance
 * suites. Each test names the wire-contract rule the emitted snippet implements.
 */
class SerdeGeneratorTest {

  private static String generateSerde(String modelText) {
    return PluginTestHarness.generate(modelText, "test.serde#Svc", "test::serde")
        .expectFileString("/src/serde.cc");
  }

  private static final String KITCHEN_MODEL =
      """
      $version: "2.0"
      namespace test.serde

      service Svc { version: "1", operations: [Op] }
      operation Op { input := { payload: Payload } }

      structure Payload {
          @required
          name: String

          dense: DenseList
          holey: SparseList
          picked: Choice

          @timestampFormat("http-date")
          seen: Timestamp
      }

      list DenseList { member: String }

      @sparse
      list SparseList { member: Integer }

      union Choice {
          left: String
          right: Integer
      }
      """;

  @Test
  void requiredMemberAbsenceNamesTheMember() {
    String serde = generateSerde(KITCHEN_MODEL);
    assertTrue(serde.contains("\"Payload: missing required member: name\""), serde);
  }

  @Test
  void denseListsRejectNullElementsAndSparseListsKeepThem() {
    String serde = generateSerde(KITCHEN_MODEL);
    // Dense: a null element is a wire error, never silently dropped.
    assertTrue(serde.contains("null element in a dense list"), serde);
    // Sparse: elements are std::optional and nulls survive both directions.
    assertTrue(serde.contains("std::vector<std::optional<std::int32_t>>"), serde);
  }

  @Test
  void unionsEnforceExactlyOneMemberToleratingTheTypeKey() {
    String serde = generateSerde(KITCHEN_MODEL);
    // The exactly-one arithmetic, __type excluded (error payloads carry it
    // next to the member) — the contract documented in generated-types.md.
    assertTrue(
        serde.contains("doc.as_map().size() - (doc.Find(\"__type\") != nullptr ? 1 : 0) != 1"),
        serde);
    assertTrue(serde.contains("expected exactly one union member"), serde);
    assertTrue(serde.contains("unknown or missing union member"), serde);
  }

  @Test
  void timestampFormatTraitOverridesTheProtocolDefault() {
    String serde = generateSerde(KITCHEN_MODEL);
    assertTrue(serde.contains("smithy::TimestampFormat::kHttpDate"), serde);
  }

  @Test
  void unknownResponseMembersAreIgnoredNotErrors() {
    // Tolerant reads: deserializers look members up by name (Find) and never
    // iterate-and-reject unknown keys — the model-evolution contract.
    String serde = generateSerde(KITCHEN_MODEL);
    assertTrue(serde.contains("doc.Find(\"name\")"), serde);
    assertFalse(serde.contains("unknown member in structure"), serde);
  }

  @Test
  void wireNameHonorsJsonNameOnlyWhenTheProtocolDoes() {
    // One policy for serde functions and binding code alike: a protocol that
    // ignores @jsonName must ignore it in BOTH, or the body splits its keys.
    var member =
        software.amazon.smithy.model.shapes.MemberShape.builder()
            .id("test.serde#Payload$renamed")
            .target("smithy.api#String")
            .addTrait(new software.amazon.smithy.model.traits.JsonNameTrait("wire_key"))
            .build();
    assertEquals("wire_key", HttpBindingCodeGen.wireName(member, true));
    assertEquals("renamed", HttpBindingCodeGen.wireName(member, false));
  }

  @Test
  void hasSerdeFunctionsMatchesExactlyTheShapeKindsSerdeEmits() {
    // The helper-name collision guard reuses this predicate, so its edges
    // matter beyond serde itself: enums convert through FromString/ToString
    // (no Serialize/Deserialize functions to hide), simple shapes inline,
    // and smithy.api#Unit never crosses the wire.
    Model model =
        Model.assembler()
            .addUnparsedModel(
                "kinds.smithy",
                """
                $version: "2.0"
                namespace test.kinds

                structure S {}
                union U { a: String }
                list L { member: String }
                map M { key: String, value: String }

                enum E {
                    A
                }

                intEnum IE {
                    A = 1
                }

                string Str
                """)
            .assemble()
            .unwrap();
    for (String serde : new String[] {"S", "U", "L", "M"}) {
      assertTrue(
          SerdeGenerator.hasSerdeFunctions(
              model.expectShape(
                  software.amazon.smithy.model.shapes.ShapeId.from("test.kinds#" + serde))),
          serde);
    }
    for (String inline : new String[] {"E", "IE", "Str"}) {
      assertFalse(
          SerdeGenerator.hasSerdeFunctions(
              model.expectShape(
                  software.amazon.smithy.model.shapes.ShapeId.from("test.kinds#" + inline))),
          inline);
    }
    assertFalse(
        SerdeGenerator.hasSerdeFunctions(
            model.expectShape(
                software.amazon.smithy.model.shapes.ShapeId.from("smithy.api#Unit"))));
  }
}
