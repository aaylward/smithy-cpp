package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * MemberDefaults decides which generated members are plain (default-initialized, always
 * serialized), which stay optional with a server-side fill, and which @required members tolerate
 * absence — the @default/@input/@required semantics matrix from the Smithy spec.
 */
class MemberDefaultsTest {

  private static Model model() {
    return Model.assembler()
        .addUnparsedModel(
            "defaults.smithy",
            """
            $version: "2.0"
            namespace test.defaults

            service Svc { version: "1", operations: [Op] }

            operation Op {
                input := {
                    limit: Integer = 25
                    tag: String
                }
            }

            structure Settings {
                retries: Integer = 3

                @required
                mode: String = "auto"

                @clientOptional
                hint: String = "h"

                nullable: String = null

                floor: Long = -9223372036854775808

                stamp: Timestamp = "2020-01-01T00:00:00Z"
            }
            """)
        .assemble()
        .unwrap();
  }

  private static MemberShape member(Model model, String id) {
    return model.expectShape(ShapeId.from(id), MemberShape.class);
  }

  @Test
  void plainDefaultsArePopulated() {
    Model m = model();
    MemberShape retries = member(m, "test.defaults#Settings$retries");
    assertTrue(MemberDefaults.populated(m, retries));
    assertTrue(MemberDefaults.plain(m, retries));
    assertFalse(MemberDefaults.fillOnParse(m, retries));
    assertFalse(MemberDefaults.lenientRequired(m, retries));
  }

  @Test
  void inputStructureMembersStayOptionalAndFillOnParse() {
    // @input (implicit on `:=` operation inputs) keeps defaulted members
    // client-optional per the spec: clients skip them, servers fill them.
    Model m = model();
    MemberShape limit = member(m, "test.defaults#OpInput$limit");
    assertFalse(MemberDefaults.populated(m, limit));
    assertTrue(MemberDefaults.fillOnParse(m, limit));
    assertFalse(MemberDefaults.plain(m, limit));
  }

  @Test
  void requiredWithDefaultIsTheEvolutionLeniencyPattern() {
    Model m = model();
    MemberShape mode = member(m, "test.defaults#Settings$mode");
    assertTrue(MemberDefaults.lenientRequired(m, mode));
    assertTrue(MemberDefaults.populated(m, mode)); // required wins over @input rules
    assertTrue(MemberDefaults.plain(m, mode));
  }

  @Test
  void clientOptionalAndNullDefaultsDoNotPopulate() {
    Model m = model();
    assertFalse(MemberDefaults.populated(m, member(m, "test.defaults#Settings$hint")));
    assertFalse(MemberDefaults.populated(m, member(m, "test.defaults#Settings$nullable")));
    // An undefaulted member is neither populated nor lenient.
    MemberShape tag = member(m, "test.defaults#OpInput$tag");
    assertFalse(MemberDefaults.populated(m, tag));
    assertFalse(MemberDefaults.fillOnParse(m, tag));
    assertFalse(MemberDefaults.plain(m, tag));
  }

  @Test
  void timestampDefaultsOnlyCountWhenNumeric() {
    // A date-time string default is not (yet) expressible as a C++
    // initializer; the member must stay optional rather than miscompile.
    Model m = model();
    assertFalse(MemberDefaults.populated(m, member(m, "test.defaults#Settings$stamp")));
  }

  @Test
  void int64MinDefaultIsStillPopulated() {
    // The value the literal layer must special-case (issue #43) is a
    // perfectly valid default; eligibility must not depend on magnitude.
    Model m = model();
    assertTrue(MemberDefaults.populated(m, member(m, "test.defaults#Settings$floor")));
  }
}
