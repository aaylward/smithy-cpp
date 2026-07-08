// Hand-written malformed-server suite for simpleRestJson (issue #48). The
// jsonrpc2 protocol tests generate an equivalent file from the model's
// httpMalformedRequestTests traits; the alloy conformance suite carries no
// such traits, so until upstream grows them this suite pins how the
// generated PizzaAdminService server rejects hostile requests — before the
// handler ever runs. Lives outside generated/ because that tree is a golden
// the codegen CI job regenerates byte-for-byte.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "smithy/json/json.h"
#include "smithy/protocoltests/simplerestjson/server.h"

namespace smithy::protocoltests::simplerestjson {
namespace {

// Counts invocations; a malformed request must be rejected before any
// operation runs, so every test asserts calls stays 0.
class RecordingHandler : public PizzaAdminServiceHandler {
 public:
  smithy::Outcome<AddMenuItemOutput> AddMenuItem(const AddMenuItemInput&) override {
    ++calls;
    return AddMenuItemOutput{};
  }
  smithy::Outcome<CustomCodeOutput> CustomCode(const CustomCodeInput&) override {
    ++calls;
    return CustomCodeOutput{};
  }
  smithy::Outcome<GetEnumOutput> GetEnum(const GetEnumInput&) override {
    ++calls;
    return GetEnumOutput{};
  }
  smithy::Outcome<GetIntEnumOutput> GetIntEnum(const GetIntEnumInput&) override {
    ++calls;
    return GetIntEnumOutput{};
  }
  smithy::Outcome<GetMenuOutput> GetMenu(const GetMenuInput&) override {
    ++calls;
    return GetMenuOutput{};
  }
  smithy::Outcome<HeaderEndpointOutput> HeaderEndpoint(const HeaderEndpointInput&) override {
    ++calls;
    return HeaderEndpointOutput{};
  }
  smithy::Outcome<HealthOutput> Health(const HealthInput&) override {
    ++calls;
    return HealthOutput{};
  }
  smithy::Outcome<HttpPayloadRequiredWithDefaultOutput> HttpPayloadRequiredWithDefault(
      const HttpPayloadRequiredWithDefaultInput&) override {
    ++calls;
    return HttpPayloadRequiredWithDefaultOutput{};
  }
  smithy::Outcome<HttpPayloadWithDefaultOutput> HttpPayloadWithDefault(
      const HttpPayloadWithDefaultInput&) override {
    ++calls;
    return HttpPayloadWithDefaultOutput{};
  }
  smithy::Outcome<OpenUnionsOutput> OpenUnions(const OpenUnionsInput&) override {
    ++calls;
    return OpenUnionsOutput{};
  }
  smithy::Outcome<RoundTripOutput> RoundTrip(const RoundTripInput&) override {
    ++calls;
    return RoundTripOutput{};
  }
  smithy::Outcome<VersionOutput> Version(const VersionInput&) override {
    ++calls;
    return VersionOutput{};
  }
  int calls = 0;
};

class SimpleRestJsonMalformedTest : public testing::Test {
 protected:
  smithy::http::HttpResponse Send(smithy::http::HttpRequest request) {
    return server_.Handler()(request);
  }

  std::shared_ptr<RecordingHandler> handler_ = std::make_shared<RecordingHandler>();
  PizzaAdminServiceServer server_{handler_};
};

TEST_F(SimpleRestJsonMalformedTest, UnknownRouteIs404) {
  smithy::http::HttpRequest request;
  request.method = "GET";
  request.target = "/no-such-route";
  EXPECT_EQ(Send(request).status, 404);
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(SimpleRestJsonMalformedTest, WrongMethodIs405) {
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/health";
  EXPECT_EQ(Send(request).status, 405);
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(SimpleRestJsonMalformedTest, UnparseableJsonBodyIsSerializationException) {
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/restaurant/r1/menu/item";
  request.headers.Set("content-type", "application/json");
  request.body = "{";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400) << response.body;
  EXPECT_EQ(response.headers.Get("x-error-type").value_or("<missing>"), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(SimpleRestJsonMalformedTest, WrongContentTypeIs415) {
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/restaurant/r1/menu/item";
  request.headers.Set("content-type", "text/plain");
  request.body = "{}";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 415) << response.body;
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(SimpleRestJsonMalformedTest, EnumLabelViolationReportsTheSuiteExactMessage) {
  smithy::http::HttpRequest request;
  request.method = "GET";
  request.target = "/get-enum/bogus";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400) << response.body;
  EXPECT_EQ(response.headers.Get("x-error-type").value_or("<missing>"), "ValidationException");
  EXPECT_EQ(handler_->calls, 0);

  const auto body = smithy::json::Decode(response.body);
  ASSERT_TRUE(body.ok()) << response.body;
  const smithy::Document* field_list = body->Find("fieldList");
  ASSERT_NE(field_list, nullptr) << response.body;
  ASSERT_EQ(field_list->as_list().size(), 1u) << response.body;
  const auto& failure = field_list->as_list()[0];
  EXPECT_EQ(failure.Find("path")->as_string(), "/aa");
  EXPECT_EQ(failure.Find("message")->as_string(),
            "Value at '/aa' failed to satisfy constraint: Member must satisfy enum value set: "
            "[v1, v2]");
}

}  // namespace
}  // namespace smithy::protocoltests::simplerestjson
