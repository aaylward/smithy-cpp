#include "smithy/client/retry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "smithy/core/error.h"

namespace smithy {
namespace {

using std::chrono::milliseconds;

// Scripted transport: pops one outcome per Send.
class ScriptedTransport final : public http::HttpClient {
 public:
  Outcome<http::HttpResponse> Send(const http::HttpRequest& request) override {
    (void)request;
    ++calls;
    if (script.empty()) return http::HttpResponse{200, {}, "fallback"};
    auto next = script.front();
    script.erase(script.begin());
    return next;
  }

  std::vector<Outcome<http::HttpResponse>> script;
  int calls = 0;
};

RetryPolicy InstantPolicy(std::vector<milliseconds>* slept) {
  RetryPolicy policy;
  policy.sleep = [slept](milliseconds d) { slept->push_back(d); };
  policy.jitter = [] { return 1.0; };  // deterministic: always the ceiling
  return policy;
}

TEST(RetryDelayTest, FullJitterExponentialWithCap) {
  RetryPolicy policy;
  policy.initial_backoff = milliseconds(100);
  policy.max_backoff = milliseconds(350);
  EXPECT_EQ(RetryDelay(policy, 1, 1.0), milliseconds(100));
  EXPECT_EQ(RetryDelay(policy, 2, 1.0), milliseconds(200));
  EXPECT_EQ(RetryDelay(policy, 3, 1.0), milliseconds(350));   // capped
  EXPECT_EQ(RetryDelay(policy, 2, 0.5), milliseconds(100));   // jitter scales
  EXPECT_EQ(RetryDelay(policy, 40, 1.0), milliseconds(350));  // huge retry: no overflow
}

TEST(RetryableStatusTest, TransientStatusesOnly) {
  for (int status : {429, 500, 502, 503, 504}) EXPECT_TRUE(RetryableStatus(status)) << status;
  for (int status : {200, 201, 204, 400, 403, 404, 501}) {
    EXPECT_FALSE(RetryableStatus(status)) << status;
  }
}

TEST(SendWithRetriesTest, RetriesTransportErrorsThenSucceeds) {
  ScriptedTransport transport;
  transport.script = {Error::Transport("refused"), Error::Transport("refused"),
                      http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept));
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome->body, "ok");
  EXPECT_EQ(transport.calls, 3);
  EXPECT_EQ(slept, (std::vector<milliseconds>{milliseconds(100), milliseconds(200)}));
}

TEST(SendWithRetriesTest, RetriesTransientStatuses) {
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{503, {}, "busy"}, http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept));
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome->status, 200);
  EXPECT_EQ(transport.calls, 2);
}

TEST(SendWithRetriesTest, DoesNotRetryClientErrors) {
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{404, {}, "nope"}};
  std::vector<milliseconds> slept;
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept));
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome->status, 404);
  EXPECT_EQ(transport.calls, 1);
  EXPECT_TRUE(slept.empty());
}

TEST(SendWithRetriesTest, GivesUpAfterMaxAttempts) {
  ScriptedTransport transport;
  transport.script = {Error::Transport("a"), Error::Transport("b"), Error::Transport("c"),
                      Error::Transport("d")};
  std::vector<milliseconds> slept;
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept));
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().message(), "c");  // the third (last) attempt's failure
  EXPECT_EQ(transport.calls, 3);
}

TEST(SendWithRetriesTest, MaxAttemptsOneDisablesRetries) {
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{503, {}, "busy"}};
  std::vector<milliseconds> slept;
  RetryPolicy policy = InstantPolicy(&slept);
  policy.max_attempts = 1;
  const auto outcome = SendWithRetries(transport, {}, policy);
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome->status, 503);
  EXPECT_EQ(transport.calls, 1);
}

}  // namespace
}  // namespace smithy
