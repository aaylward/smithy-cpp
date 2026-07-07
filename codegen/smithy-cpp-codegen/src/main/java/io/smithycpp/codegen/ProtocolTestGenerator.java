package io.smithycpp.codegen;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.TreeMap;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.protocoltests.traits.AppliesTo;
import software.amazon.smithy.protocoltests.traits.HttpRequestTestCase;
import software.amazon.smithy.protocoltests.traits.HttpRequestTestsTrait;
import software.amazon.smithy.protocoltests.traits.HttpResponseTestCase;
import software.amazon.smithy.protocoltests.traits.HttpResponseTestsTrait;

/**
 * Generates GoogleTest conformance suites from the official {@code smithy.test#httpRequestTests} /
 * {@code #httpResponseTests} traits (smithy-rs's {@code ClientProtocolTestGenerator} pattern,
 * including its must-shrink exclusion list in {@code protocol-test-exclusions.txt}).
 *
 * <p>Emits {@code tests/request_tests.cc}, {@code tests/response_tests.cc} and a {@code
 * tests/BUILD.bazel} into the generated module.
 */
final class ProtocolTestGenerator {

  private static final String EXCLUSIONS_RESOURCE = "protocol-test-exclusions.txt";

  private final CppContext context;
  private final ServiceShape service;
  private final ProtocolGenerator protocol;
  private final List<OperationShape> operations;
  private final NodeLiteralGenerator literals;

  /** "kind testId" -> reason, for this service only (kind: request|response|error|any). */
  private final Map<String, String> exclusions;

  private final Map<String, String> unusedExclusions;
  private final List<String> excludedHere = new ArrayList<>();

  ProtocolTestGenerator(
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> operations) {
    this.context = context;
    this.service = service;
    this.protocol = protocol;
    this.operations = operations;
    this.literals = new NodeLiteralGenerator(context);
    this.exclusions = loadExclusions(service.getId());
    this.unusedExclusions = new LinkedHashMap<>(exclusions);
  }

  void run() {
    writeRequestTests();
    writeResponseTests();
    if (!unusedExclusions.isEmpty()) {
      throw new CodegenException(
          "cpp-codegen: protocol-test-exclusions.txt has entries that matched no generated test"
              + " for "
              + service.getId()
              + " (remove them): "
              + unusedExclusions.keySet());
    }
  }

  private String clientType() {
    return CppReservedWords.escape(service.getId().getName()) + "Client";
  }

  private boolean excluded(String kind, String testId) {
    for (String key : new String[] {kind + " " + testId, "any " + testId}) {
      String reason = exclusions.get(key);
      if (reason != null) {
        unusedExclusions.remove(key);
        excludedHere.add(testId + " (" + kind + ") — " + reason);
        return true;
      }
    }
    return false;
  }

  private boolean isThisProtocol(ShapeId testProtocol) {
    return testProtocol.equals(protocol.traitId());
  }

  // ---------------------------------------------------------------------
  // Request tests
  // ---------------------------------------------------------------------

  private void writeRequestTests() {
    context
        .writerDelegator()
        .useFileWriter(
            "tests/request_tests.cc",
            w -> {
              List<String> tests = new ArrayList<>();
              for (OperationShape operation : operations) {
                Optional<HttpRequestTestsTrait> trait =
                    operation.getTrait(HttpRequestTestsTrait.class);
                if (trait.isEmpty()) {
                  continue;
                }
                for (HttpRequestTestCase testCase : trait.get().getTestCasesFor(AppliesTo.CLIENT)) {
                  if (!isThisProtocol(testCase.getProtocol())
                      || excluded("request", testCase.getId())) {
                    continue;
                  }
                  tests.add(requestTest(operation, testCase));
                }
              }
              writeCommonIncludes(w);
              w.write("// Generated from smithy.test#httpRequestTests (client cases).");
              writeExcludedComment(w);
              writeFixture(w);
              for (String test : tests) {
                w.writeWithNoFormatting(test);
                w.write("");
              }
            });
  }

  private String requestTest(OperationShape operation, HttpRequestTestCase testCase) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StringBuilder t = new StringBuilder();
    testCase
        .getDocumentation()
        .ifPresent(docs -> docs.lines().forEach(line -> t.append("// ").append(line).append('\n')));
    t.append("TEST(")
        .append(service.getId().getName())
        .append("RequestTest, ")
        .append(testCase.getId())
        .append(") {\n");
    // A test host with a path (e.g. "example.com/custom") must prefix the
    // request target, so it flows in as the configured endpoint.
    String endpoint =
        testCase.getHost().map(host -> CppLiterals.stringLiteral("http://" + host)).orElse("");
    t.append("  Fixture fixture = MakeFixture(").append(endpoint).append(");\n");
    t.append("  const ")
        .append(context.cppSymbols().toSymbol(input).getName())
        .append(" input = ")
        .append(literals.expression(input, testCase.getParams()))
        .append(";\n");
    t.append("  (void)fixture.client.")
        .append(CppReservedWords.escape(operation.getId().getName()))
        .append("(input);\n");
    t.append("  const smithy::http::HttpRequest& request = fixture.transport->last_request;\n");
    t.append("  EXPECT_EQ(request.method, ")
        .append(CppLiterals.stringLiteral(testCase.getMethod()))
        .append(");\n");
    t.append("  EXPECT_EQ(smithy::testing::UriPath(request.target), ")
        .append(CppLiterals.stringLiteral(testCase.getUri()))
        .append(");\n");
    if (!testCase.getQueryParams().isEmpty()) {
      t.append("  EXPECT_TRUE(smithy::testing::QueryContains(request.target, ")
          .append(stringVector(testCase.getQueryParams()))
          .append("));\n");
    }
    if (!testCase.getForbidQueryParams().isEmpty()) {
      t.append("  EXPECT_TRUE(smithy::testing::QueryForbidsKeys(request.target, ")
          .append(stringVector(testCase.getForbidQueryParams()))
          .append("));\n");
    }
    if (!testCase.getRequireQueryParams().isEmpty()) {
      t.append("  EXPECT_TRUE(smithy::testing::QueryRequiresKeys(request.target, ")
          .append(stringVector(testCase.getRequireQueryParams()))
          .append("));\n");
    }
    for (Map.Entry<String, String> header : new TreeMap<>(testCase.getHeaders()).entrySet()) {
      t.append("  EXPECT_EQ(request.headers.Get(")
          .append(CppLiterals.stringLiteral(header.getKey()))
          .append(").value_or(\"<missing>\"), ")
          .append(CppLiterals.stringLiteral(header.getValue()))
          .append(");\n");
    }
    for (String name : testCase.getForbidHeaders()) {
      t.append("  EXPECT_FALSE(request.headers.Has(")
          .append(CppLiterals.stringLiteral(name))
          .append("));\n");
    }
    for (String name : testCase.getRequireHeaders()) {
      t.append("  EXPECT_TRUE(request.headers.Has(")
          .append(CppLiterals.stringLiteral(name))
          .append("));\n");
    }
    testCase
        .getBody()
        .ifPresent(body -> t.append(bodyAssertion(body, testCase.getBodyMediaType())));
    return t.append("}").toString();
  }

  private String bodyAssertion(String body, Optional<String> mediaType) {
    if (body.isEmpty()) {
      return "  EXPECT_TRUE(request.body.empty()) << request.body;\n";
    }
    String type = mediaType.orElse("");
    if (type.equals("application/json") || type.endsWith("+json")) {
      return "  EXPECT_TRUE(smithy::testing::JsonBodyEquals("
          + CppLiterals.stringLiteral(body)
          + ", request.body));\n";
    }
    if (type.equals("application/cbor")) {
      return "  EXPECT_TRUE(smithy::testing::CborBodyEqualsBase64("
          + CppLiterals.stringLiteral(body)
          + ", request.body));\n";
    }
    return "  EXPECT_EQ(request.body, " + CppLiterals.stringLiteral(body) + ");\n";
  }

  // ---------------------------------------------------------------------
  // Response tests (output cases + error cases)
  // ---------------------------------------------------------------------

  private void writeResponseTests() {
    context
        .writerDelegator()
        .useFileWriter(
            "tests/response_tests.cc",
            w -> {
              List<String> tests = new ArrayList<>();
              for (OperationShape operation : operations) {
                operation
                    .getTrait(HttpResponseTestsTrait.class)
                    .ifPresent(
                        trait -> {
                          for (HttpResponseTestCase testCase :
                              trait.getTestCasesFor(AppliesTo.CLIENT)) {
                            if (!isThisProtocol(testCase.getProtocol())
                                || excluded("response", testCase.getId())) {
                              continue;
                            }
                            tests.add(responseTest(operation, testCase, null));
                          }
                        });
              }
              // Error cases run against the (alphabetically) first operation that
              // declares the error, so each case is generated exactly once.
              Map<String, OperationShape> firstDeclaringOp = new TreeMap<>();
              Map<String, StructureShape> errorByName = new TreeMap<>();
              for (OperationShape operation : operations) {
                for (ShapeId errorId : operation.getErrors(service)) {
                  StructureShape error =
                      context.model().expectShape(errorId).asStructureShape().orElseThrow();
                  String name = context.cppSymbols().toSymbol(error).getName();
                  firstDeclaringOp.putIfAbsent(name, operation);
                  errorByName.putIfAbsent(name, error);
                }
              }
              for (Map.Entry<String, StructureShape> entry : errorByName.entrySet()) {
                StructureShape error = entry.getValue();
                OperationShape operation = firstDeclaringOp.get(entry.getKey());
                Optional<HttpResponseTestsTrait> trait =
                    error.getTrait(HttpResponseTestsTrait.class);
                if (trait.isEmpty()) {
                  continue;
                }
                for (HttpResponseTestCase testCase :
                    trait.get().getTestCasesFor(AppliesTo.CLIENT)) {
                  if (!isThisProtocol(testCase.getProtocol())
                      || excluded("error", testCase.getId())) {
                    continue;
                  }
                  tests.add(responseTest(operation, testCase, error));
                }
              }
              writeCommonIncludes(w);
              w.write("// Generated from smithy.test#httpResponseTests (client cases),");
              w.write("// including the cases attached to modeled error shapes.");
              writeExcludedComment(w);
              writeFixture(w);
              for (String test : tests) {
                w.writeWithNoFormatting(test);
                w.write("");
              }
            });
  }

  /** One response test; {@code error} null for output cases. */
  private String responseTest(
      OperationShape operation, HttpResponseTestCase testCase, StructureShape error) {
    StringBuilder t = new StringBuilder();
    testCase
        .getDocumentation()
        .ifPresent(docs -> docs.lines().forEach(line -> t.append("// ").append(line).append('\n')));
    t.append("TEST(")
        .append(service.getId().getName())
        .append(error == null ? "ResponseTest, " : "ErrorTest, ")
        .append(testCase.getId())
        .append(") {\n");
    t.append("  Fixture fixture = MakeFixture();\n");
    t.append("  fixture.transport->next_response.status = ")
        .append(testCase.getCode())
        .append(";\n");
    for (Map.Entry<String, String> header : new TreeMap<>(testCase.getHeaders()).entrySet()) {
      t.append("  fixture.transport->next_response.headers.Set(")
          .append(CppLiterals.stringLiteral(header.getKey()))
          .append(", ")
          .append(CppLiterals.stringLiteral(header.getValue()))
          .append(");\n");
    }
    String body = testCase.getBody().orElse("");
    String mediaType = testCase.getBodyMediaType().orElse("");
    if (mediaType.equals("application/cbor")) {
      t.append("  fixture.transport->next_response.body = smithy::testing::FromBase64(")
          .append(CppLiterals.stringLiteral(body))
          .append(");\n");
    } else {
      t.append("  fixture.transport->next_response.body = ")
          .append(CppLiterals.stringLiteral(body))
          .append(";\n");
    }
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    t.append("  const auto outcome = fixture.client.")
        .append(CppReservedWords.escape(operation.getId().getName()))
        .append("(")
        .append(context.cppSymbols().toSymbol(input).getName())
        .append("{});\n");
    if (error == null) {
      StructureShape output = ProtocolSupport.outputShape(context, operation);
      t.append("  ASSERT_TRUE(outcome.ok()) << outcome.error().message();\n");
      t.append("  const ")
          .append(context.cppSymbols().toSymbol(output).getName())
          .append(" expected = ")
          .append(literals.expression(output, testCase.getParams()))
          .append(";\n");
      t.append("  EXPECT_EQ(*outcome, expected);\n");
    } else {
      String errorType = context.cppSymbols().toSymbol(error).getName();
      t.append("  ASSERT_FALSE(outcome.ok());\n");
      // code() carries the wire-level shape name (not the C++ type name).
      t.append("  EXPECT_EQ(outcome.error().code(), ")
          .append(CppLiterals.stringLiteral(error.getId().getName()))
          .append(");\n");
      t.append("  const auto* detail = outcome.error().detail<").append(errorType).append(">();\n");
      t.append("  ASSERT_NE(detail, nullptr);\n");
      t.append("  const ")
          .append(errorType)
          .append(" expected = ")
          .append(literals.expression(error, testCase.getParams()))
          .append(";\n");
      t.append("  EXPECT_EQ(*detail, expected);\n");
    }
    return t.append("}").toString();
  }

  // ---------------------------------------------------------------------
  // Shared emission
  // ---------------------------------------------------------------------

  private void writeCommonIncludes(CppWriter w) {
    w.addInclude("<gtest/gtest.h>");
    w.addInclude("<cstdint>");
    w.addInclude("<limits>");
    w.addInclude("<memory>");
    w.addInclude("<optional>");
    w.addInclude("<string>");
    w.addInclude("<utility>");
    w.addInclude("\"" + context.settings().includePrefix() + "/client.h\"");
    w.addInclude("\"smithy/testing/protocol_test.h\"");
  }

  private void writeExcludedComment(CppWriter w) {
    if (excludedHere.isEmpty()) {
      return;
    }
    w.write("//");
    w.write("// Excluded cases (protocol-test-exclusions.txt; the list must only shrink):");
    for (String line : excludedHere) {
      w.writeWithNoFormatting("//   " + line);
    }
    excludedHere.clear();
    w.write("");
  }

  private void writeFixture(CppWriter w) {
    String client = clientType();
    w.write("namespace {");
    w.write("");
    w.openBlock("struct Fixture {");
    w.write("std::shared_ptr<smithy::testing::CapturingTransport> transport;");
    w.write("$L client;", client);
    w.closeBlock("};");
    w.write("");
    w.openBlock("Fixture MakeFixture(const std::string& endpoint = \"\") {");
    w.write("auto transport = std::make_shared<smithy::testing::CapturingTransport>();");
    w.write("smithy::ClientConfig config;");
    w.write("config.http_client = transport;");
    w.write("config.endpoint = endpoint;");
    w.write("// Create cannot fail when a transport is injected.");
    w.write("auto client = $L::Create(std::move(config));", client);
    w.write("return Fixture{std::move(transport), *std::move(client)};");
    w.closeBlock("}");
    w.write("");
    w.write("}  // namespace");
    w.write("");
  }

  private static String stringVector(List<String> values) {
    StringBuilder out = new StringBuilder("{");
    for (int i = 0; i < values.size(); i++) {
      if (i > 0) {
        out.append(", ");
      }
      out.append(CppLiterals.stringLiteral(values.get(i)));
    }
    return out.append('}').toString();
  }

  /** Entries for {@code serviceId} from the shared exclusion resource. */
  private static Map<String, String> loadExclusions(ShapeId serviceId) {
    Map<String, String> exclusions = new LinkedHashMap<>();
    try (InputStream stream =
        ProtocolTestGenerator.class.getResourceAsStream(EXCLUSIONS_RESOURCE)) {
      if (stream == null) {
        return exclusions;
      }
      BufferedReader reader =
          new BufferedReader(new InputStreamReader(stream, StandardCharsets.UTF_8));
      String line;
      while ((line = reader.readLine()) != null) {
        line = line.trim();
        if (line.isEmpty() || line.startsWith("#")) {
          continue;
        }
        String[] parts = line.split("\\s+", 4);
        if (parts.length < 4 || !parts[1].matches("request|response|error|any")) {
          throw new CodegenException(
              "cpp-codegen: bad exclusion line (want '<service> <request|response|error|any>"
                  + " <testId> <reason>'): "
                  + line);
        }
        if (parts[0].equals(serviceId.toString())) {
          exclusions.put(parts[1] + " " + parts[2], parts[3]);
        }
      }
    } catch (IOException e) {
      throw new CodegenException("cpp-codegen: failed to read " + EXCLUSIONS_RESOURCE, e);
    }
    return exclusions;
  }
}
