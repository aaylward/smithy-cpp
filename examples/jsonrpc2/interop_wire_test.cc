// Interop checks for smithy.cpp.protocols#jsonRpc2: JSON-RPC 2.0 is an open
// standard, so the generated code must talk to peers that were never
// generated from the model. A hand-rolled client (raw envelopes, string ids)
// drives the generated server, and the generated client drives a hand-rolled
// JSON-RPC responder.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "example/calculator/client.h"
#include "example/calculator/server.h"
#include "smithy/client/config.h"
#include "smithy/core/document.h"
#include "smithy/http/transport.h"
#include "smithy/json/json.h"

namespace example::calculator {
namespace {

class Handler final : public CalculatorHandler {
 public:
  smithy::Outcome<AddOutput> Add(const AddInput& input) override {
    return AddOutput{.sum = input.a + input.b};
  }

  smithy::Outcome<DivideOutput> Divide(const DivideInput& input) override {
    if (input.divisor == 0) {
      smithy::Error error = smithy::Error::Modeled("DivisionByZero", "division by zero");
      error.set_detail(DivisionByZero{.message = "division by zero"});
      return error;
    }
    return DivideOutput{.quotient = input.dividend / input.divisor};
  }
};

smithy::http::HttpResponse Call(const std::string& body) {
  CalculatorServer server(std::make_shared<Handler>());
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/";
  request.headers.Set("content-type", "application/json");
  request.body = body;
  return server.Handler()(request);
}

// A foreign JSON-RPC client is free to pick any id type; the server echoes
// it back verbatim (the generated client always sends 1, so this only shows
// up in interop).
TEST(JsonRpc2InteropTest, HandRolledCallWithStringIdRoundTrips) {
  const auto response =
      Call(R"({"jsonrpc":"2.0","method":"Add","params":{"a":2,"b":3},"id":"abc-123"})");
  EXPECT_EQ(response.status, 200);
  auto doc = smithy::json::Decode(response.body);
  ASSERT_TRUE(doc.ok()) << response.body;
  ASSERT_TRUE(doc->is_map());
  EXPECT_EQ(doc->Find("jsonrpc")->as_string(), "2.0");
  EXPECT_EQ(doc->Find("id")->as_string(), "abc-123");
  const smithy::Document* result = doc->Find("result");
  ASSERT_NE(result, nullptr) << response.body;
  EXPECT_EQ(result->Find("sum")->AsNumber(), 5.0);
}

TEST(JsonRpc2InteropTest, ModeledErrorsArriveAsJsonRpcErrorObjects) {
  const auto response =
      Call(R"({"jsonrpc":"2.0","method":"Divide","params":{"dividend":1,"divisor":0},"id":7})");
  EXPECT_EQ(response.status, 200);  // JSON-RPC errors are HTTP 200 envelopes.
  auto doc = smithy::json::Decode(response.body);
  ASSERT_TRUE(doc.ok()) << response.body;
  const smithy::Document* error = doc->Find("error");
  ASSERT_NE(error, nullptr) << response.body;
  EXPECT_EQ(error->Find("code")->as_int(), 422);  // @httpError(422)
  EXPECT_EQ(error->Find("message")->as_string(), "division by zero");
  const smithy::Document* data = error->Find("data");
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->Find("__type")->as_string(), "example.calculator#DivisionByZero");
  EXPECT_EQ(doc->Find("id")->as_int(), 7);
}

// A JSON-RPC responder that was never generated from the model: it dispatches
// on the envelope itself, the way any off-the-shelf JSON-RPC library would.
class HandRolledPeer final : public smithy::http::HttpClient {
 public:
  smithy::Outcome<smithy::http::HttpResponse> Send(
      const smithy::http::HttpRequest& request) override {
    auto doc = smithy::json::Decode(request.body);
    if (!doc.ok() || !doc->is_map()) return smithy::http::HttpResponse{400, {}, ""};
    last_envelope = *std::move(doc);
    const std::string& method = last_envelope.Find("method")->as_string();
    std::string result;
    if (method == "Add") {
      const smithy::Document* params = last_envelope.Find("params");
      const double sum = params->Find("a")->AsNumber() + params->Find("b")->AsNumber();
      result = R"({"jsonrpc":"2.0","result":{"sum":)" + std::to_string(sum) + R"(},"id":1})";
    } else {
      result = R"({"jsonrpc":"2.0","error":{"code":422,"message":"nope","data":)"
               R"({"__type":"example.calculator#DivisionByZero","message":"nope"}},"id":1})";
    }
    smithy::http::HttpResponse response{200, {}, std::move(result)};
    response.headers.Set("content-type", "application/json");
    return response;
  }

  smithy::Document last_envelope;
};

TEST(JsonRpc2InteropTest, GeneratedClientTalksToAHandRolledPeer) {
  auto peer = std::make_shared<HandRolledPeer>();
  smithy::ClientConfig config;
  config.http_client = peer;
  auto client = CalculatorClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  const auto added = client->Add(AddInput{.a = 20, .b = 22});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->sum, 42.0);
  // The request the peer saw is a plain JSON-RPC 2.0 call.
  EXPECT_EQ(peer->last_envelope.Find("jsonrpc")->as_string(), "2.0");
  EXPECT_EQ(peer->last_envelope.Find("method")->as_string(), "Add");
  EXPECT_EQ(peer->last_envelope.Find("id")->as_int(), 1);

  const auto divided = client->Divide(DivideInput{.dividend = 1, .divisor = 0});
  ASSERT_FALSE(divided.ok());
  EXPECT_EQ(divided.error().code(), "DivisionByZero");
  ASSERT_NE(divided.error().detail<DivisionByZero>(), nullptr);
  EXPECT_EQ(divided.error().detail<DivisionByZero>()->message, "nope");
}

}  // namespace
}  // namespace example::calculator
