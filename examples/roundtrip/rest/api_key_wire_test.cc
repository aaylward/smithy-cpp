// Wire-level checks for @httpApiKeyAuth(in: "query"): the generated client
// appends the key to the target, taking the '?' branch when the operation
// produced no query string and the '&' branch when it did — and the key
// itself is percent-encoded on the way.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "example/roundtrip/rest/client.h"
#include "smithy/client/config.h"
#include "smithy/http/transport.h"

namespace example::roundtrip::rest {
namespace {

class CapturingTransport final : public smithy::http::HttpClient {
 public:
  smithy::Outcome<smithy::http::HttpResponse> Send(
      const smithy::http::HttpRequest& request) override {
    last_request = request;
    return smithy::http::HttpResponse{200, {}, ""};
  }

  smithy::http::HttpRequest last_request;
};

RoundTripRestClient MakeClient(std::shared_ptr<CapturingTransport> transport) {
  smithy::ClientConfig config;
  config.http_client = std::move(transport);
  config.api_key = [] { return std::string("k&e y"); };  // needs encoding
  return *RoundTripRestClient::Create(std::move(config));
}

TEST(ApiKeyQueryTest, StartsTheQueryStringWhenThereIsNone) {
  auto transport = std::make_shared<CapturingTransport>();
  auto client = MakeClient(transport);
  (void)client.DescribeSink(DescribeSinkInput{.sinkId = "s1"});
  EXPECT_EQ(transport->last_request.target, "/sinks/s1?api-key=k%26e%20y");
}

TEST(ApiKeyQueryTest, AppendsToAnExistingQueryString) {
  auto transport = std::make_shared<CapturingTransport>();
  auto client = MakeClient(transport);
  (void)client.PutSink(PutSinkInput{.sinkId = "s1", .tag = "prod"});
  EXPECT_EQ(transport->last_request.target, "/sinks/s1?tag=prod&api-key=k%26e%20y");
}

TEST(ApiKeyQueryTest, AbsentProviderLeavesTheTargetAlone) {
  auto transport = std::make_shared<CapturingTransport>();
  smithy::ClientConfig config;
  config.http_client = transport;
  auto client = RoundTripRestClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());
  (void)client->DescribeSink(DescribeSinkInput{.sinkId = "s1"});
  EXPECT_EQ(transport->last_request.target, "/sinks/s1");
}

}  // namespace
}  // namespace example::roundtrip::rest
