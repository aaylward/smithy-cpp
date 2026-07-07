#include "smithy/server/middleware.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace smithy::server {
namespace {

using std::chrono::milliseconds;

http::HttpResponse Ok(const std::string& body) {
  http::HttpResponse response;
  response.status = 200;
  response.body = body;
  return response;
}

// Middleware appending a tag on the way in (request seen) and out (response).
Middleware Tag(std::vector<std::string>* log, const std::string& name) {
  return [log, name](http::RequestHandler next) {
    return [log, name, next](const http::HttpRequest& request) {
      log->push_back(name + ":in");
      http::HttpResponse response = next(request);
      log->push_back(name + ":out");
      return response;
    };
  };
}

TEST(ChainTest, FirstMiddlewareIsOutermost) {
  std::vector<std::string> log;
  auto handler = Chain({Tag(&log, "a"), Tag(&log, "b")}, [&](const http::HttpRequest&) {
    log.push_back("handler");
    return Ok("done");
  });

  const auto response = handler({});
  EXPECT_EQ(response.body, "done");
  EXPECT_EQ(log, (std::vector<std::string>{"a:in", "b:in", "handler", "b:out", "a:out"}));
}

TEST(ChainTest, EmptyChainIsTheHandler) {
  auto handler = Chain({}, [](const http::HttpRequest&) { return Ok("plain"); });
  EXPECT_EQ(handler({}).body, "plain");
}

TEST(ChainTest, MiddlewareCanShortCircuit) {
  bool reached = false;
  auto reject = [](http::RequestHandler) {
    return [](const http::HttpRequest&) {
      http::HttpResponse response;
      response.status = 401;
      return response;
    };
  };
  auto handler = Chain({reject}, [&](const http::HttpRequest&) {
    reached = true;
    return Ok("never");
  });

  EXPECT_EQ(handler({}).status, 401);
  EXPECT_FALSE(reached);
}

TEST(ObserveTest, ReportsMethodTargetStatusAndDuration) {
  std::vector<RequestObservation> observations;
  auto clock_time = std::chrono::steady_clock::time_point{};
  auto now = [&clock_time] {
    auto current = clock_time;
    clock_time += milliseconds(7);  // each call advances: duration = 7ms
    return current;
  };

  auto handler =
      Chain({Observe([&](const RequestObservation& o) { observations.push_back(o); }, now)},
            [](const http::HttpRequest&) {
              http::HttpResponse response;
              response.status = 404;
              return response;
            });

  http::HttpRequest request;
  request.method = "GET";
  request.target = "/cities/1";
  (void)handler(request);

  ASSERT_EQ(observations.size(), 1u);
  EXPECT_EQ(observations[0].method, "GET");
  EXPECT_EQ(observations[0].target, "/cities/1");
  EXPECT_EQ(observations[0].status, 404);
  EXPECT_EQ(observations[0].duration, milliseconds(7));
}

TEST(RequireBearerAuthTest, ValidatesTheBearerToken) {
  auto handler = Chain({RequireBearerAuth([](const std::string& token) { return token == "s3"; })},
                       [](const http::HttpRequest&) { return Ok("in"); });

  http::HttpRequest request;
  EXPECT_EQ(handler(request).status, 401);  // no header

  request.headers.Set("authorization", "Bearer s3");
  EXPECT_EQ(handler(request).status, 200);

  request.headers.Set("authorization", "bearer s3");  // scheme is case-insensitive
  EXPECT_EQ(handler(request).status, 200);

  request.headers.Set("authorization", "Bearer nope");
  EXPECT_EQ(handler(request).status, 401);

  request.headers.Set("authorization", "Basic s3");  // wrong scheme
  EXPECT_EQ(handler(request).status, 401);

  request.headers.Set("authorization", "Bearer");  // scheme without credential
  EXPECT_EQ(handler(request).status, 401);
}

TEST(RequireApiKeyHeaderTest, ValidatesTheNamedHeader) {
  auto handler = Chain(
      {RequireApiKeyHeader("x-api-key", "", [](const std::string& key) { return key == "k"; })},
      [](const http::HttpRequest&) { return Ok("in"); });

  http::HttpRequest request;
  EXPECT_EQ(handler(request).status, 401);
  request.headers.Set("x-api-key", "k");
  EXPECT_EQ(handler(request).status, 200);
  request.headers.Set("x-api-key", "wrong");
  EXPECT_EQ(handler(request).status, 401);
}

TEST(RequireApiKeyHeaderTest, SchemePrefixesTheKey) {
  auto handler = Chain({RequireApiKeyHeader("authorization", "ApiKey",
                                            [](const std::string& key) { return key == "k"; })},
                       [](const http::HttpRequest&) { return Ok("in"); });

  http::HttpRequest request;
  request.headers.Set("authorization", "ApiKey k");
  EXPECT_EQ(handler(request).status, 200);
  request.headers.Set("authorization", "k");  // missing scheme
  EXPECT_EQ(handler(request).status, 401);
}

TEST(ObserveTest, CountsEveryRequest) {
  int count = 0;
  auto handler = Chain({Observe([&](const RequestObservation&) { ++count; })},
                       [](const http::HttpRequest&) { return Ok("ok"); });
  (void)handler({});
  (void)handler({});
  (void)handler({});
  EXPECT_EQ(count, 3);
}

}  // namespace
}  // namespace smithy::server
