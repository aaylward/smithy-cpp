#include "smithy/client/retry.h"

#include <algorithm>
#include <random>
#include <thread>

namespace smithy {
namespace {

double UniformJitter() {
  thread_local std::mt19937 engine{std::random_device{}()};
  return std::uniform_real_distribution<double>(0.0, 1.0)(engine);
}

}  // namespace

std::chrono::milliseconds RetryDelay(const RetryPolicy& policy, int retry, double jitter01) {
  // Cap the exponent so the shift below cannot overflow; max_backoff clamps
  // the result long before that anyway.
  const int exponent = std::min(retry - 1, 20);
  const auto ceiling =
      std::min(policy.max_backoff, policy.initial_backoff * (std::int64_t{1} << exponent));
  return std::chrono::milliseconds(
      static_cast<std::int64_t>(static_cast<double>(ceiling.count()) * jitter01));
}

bool RetryableStatus(int status) {
  return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

Outcome<http::HttpResponse> SendWithRetries(http::HttpClient& transport,
                                            const http::HttpRequest& request,
                                            const RetryPolicy& policy) {
  const auto sleep = policy.sleep != nullptr ? policy.sleep : [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  const auto jitter = policy.jitter != nullptr ? policy.jitter : UniformJitter;
  const int attempts = std::max(policy.max_attempts, 1);

  Outcome<http::HttpResponse> outcome = transport.Send(request);
  for (int retry = 1; retry < attempts; ++retry) {
    const bool retryable =
        outcome.ok() ? RetryableStatus(outcome->status) : outcome.error().retryable();
    if (!retryable) {
      return outcome;
    }
    sleep(RetryDelay(policy, retry, jitter()));
    outcome = transport.Send(request);
  }
  return outcome;
}

}  // namespace smithy
