package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;
import software.amazon.smithy.build.PluginContext;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.node.Node;

/**
 * Direct assertions on the serde SerdeGenerator/SerdeCodeGen emit for a purpose-built model —
 * previously these behaviors were pinned only transitively, by goldens and the C++ conformance
 * suites. Each test names the wire-contract rule the emitted snippet implements.
 */
class SerdeGeneratorTest {

  private static String generateSerde(String modelText) {
    Model model =
        Model.assembler().addUnparsedModel("serde-test.smithy", modelText).assemble().unwrap();
    MockManifest manifest = new MockManifest();
    PluginContext context =
        PluginContext.builder()
            .fileManifest(manifest)
            .model(model)
            .settings(
                Node.objectNodeBuilder()
                    .withMember("service", "test.serde#Svc")
                    .withMember("namespace", "test::serde")
                    .build())
            .build();
    new CppCodegenPlugin().execute(context);
    return manifest.expectFileString("/src/serde.cc");
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
}
