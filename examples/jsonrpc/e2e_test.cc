// End-to-end test for the vendor-neutral JSON-RPC 2.0 protocol: the generated
// KeyValue client calls the generated server over loopback. Exercises the
// single-endpoint dispatch (method field), success round-trips, a modeled
// error (delivered as a JSON-RPC error object, HTTP 200), and idempotency
// token auto-fill.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "example/keyvalue/client.h"
#include "example/keyvalue/server.h"
#include "smithy/client/config.h"
#include "smithy/http/loopback.h"
#include "smithy/json/json.h"

namespace example::keyvalue {
namespace {

class Store final : public KeyValueHandler {
 public:
  smithy::Outcome<GetValueOutput> GetValue(const GetValueInput& input) override {
    const auto it = data_.find(input.key);
    if (it == data_.end()) {
      smithy::Error error = smithy::Error::Modeled("NoSuchKey", "no such key: " + input.key);
      error.set_detail(NoSuchKey{.message = "no such key: " + input.key, .key = input.key});
      return error;
    }
    return GetValueOutput{.key = input.key, .value = it->second};
  }

  smithy::Outcome<PutValueOutput> PutValue(const PutValueInput& input) override {
    data_[input.key] = input.value;
    last_request_id_ = input.requestId.value_or("");
    return PutValueOutput{.key = input.key};
  }

  std::map<std::string, std::string> data_;
  std::string last_request_id_;
};

class JsonRpcTest : public testing::Test {
 protected:
  void SetUp() override {
    handler_ = std::make_shared<Store>();
    server_ = std::make_unique<KeyValueServer>(handler_);
    auto loopback = std::make_shared<smithy::http::Loopback>();
    ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
    smithy::ClientConfig config;
    config.http_client = loopback;
    auto client = KeyValueClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<KeyValueClient>(std::move(*client));
  }

  std::shared_ptr<Store> handler_;
  std::unique_ptr<KeyValueServer> server_;
  std::unique_ptr<KeyValueClient> client_;
};

TEST_F(JsonRpcTest, PutThenGetRoundTrips) {
  const auto put = client_->PutValue(PutValueInput{.key = "color", .value = "green"});
  ASSERT_TRUE(put.ok()) << put.error().message();
  EXPECT_EQ(put->key, "color");

  const auto got = client_->GetValue(GetValueInput{.key = "color"});
  ASSERT_TRUE(got.ok()) << got.error().message();
  EXPECT_EQ(got->value, "green");
}

TEST_F(JsonRpcTest, ModeledErrorArrivesAsJsonRpcError) {
  const auto got = client_->GetValue(GetValueInput{.key = "missing"});
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error().kind(), smithy::ErrorKind::kModeled);
  EXPECT_EQ(got.error().code(), "NoSuchKey");
  EXPECT_EQ(got.error().message(), "no such key: missing");
  const auto* detail = got.error().detail<NoSuchKey>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->key, "missing");
}

TEST_F(JsonRpcTest, IdempotencyTokenIsAutoFilled) {
  (void)client_->PutValue(PutValueInput{.key = "k", .value = "v"});
  EXPECT_FALSE(handler_->last_request_id_.empty());  // client generated one
}

TEST_F(JsonRpcTest, ConstraintValidationRejectsBeforeHandler) {
  // key has @length(min:1); an empty key must be rejected by the server.
  const auto got = client_->GetValue(GetValueInput{.key = ""});
  ASSERT_FALSE(got.ok());
}

TEST_F(JsonRpcTest, UnknownMethodIsMethodNotFound) {
  // Hand-craft a JSON-RPC request for a method the service doesn't define.
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/";
  request.body = R"({"jsonrpc":"2.0","method":"Nope","params":{},"id":7})";
  const auto response = server_->Handler()(request);
  EXPECT_EQ(response.status, 200);  // JSON-RPC errors ride in the body
  const auto doc = smithy::json::Decode(response.body);
  ASSERT_TRUE(doc.ok());
  const auto* error = doc->Find("error");
  ASSERT_NE(error, nullptr);
  const auto* code = error->Find("code");
  ASSERT_NE(code, nullptr);
  EXPECT_EQ(code->as_int(), -32601);  // method not found
}

}  // namespace
}  // namespace example::keyvalue
