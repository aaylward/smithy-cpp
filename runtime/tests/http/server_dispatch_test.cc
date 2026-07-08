#include "smithy/http/server_dispatch.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "smithy/http/message.h"
#include "smithy/http/transport.h"

namespace smithy::http {
namespace {

HttpRequest SampleRequest() {
  HttpRequest request;
  request.method = "POST";
  request.target = "/tasks";
  return request;
}

TEST(ServerDispatchTest, PassesThroughASuccessfulResponse) {
  RequestHandler handler = [](const HttpRequest&) {
    HttpResponse response;
    response.status = 201;
    response.body = "ok";
    return response;
  };
  const HttpResponse response = InvokeHandlerGuarded(handler, SampleRequest());
  EXPECT_EQ(response.status, 201);
  EXPECT_EQ(response.body, "ok");
  EXPECT_FALSE(response.headers.Get("x-correlation-id").has_value());
}

TEST(ServerDispatchTest, StdExceptionBecomesA500WithCorrelationId) {
  RequestHandler handler = [](const HttpRequest&) -> HttpResponse {
    throw std::out_of_range("boom");
  };
  const HttpResponse response = InvokeHandlerGuarded(handler, SampleRequest());
  EXPECT_EQ(response.status, 500);
  const auto id = response.headers.Get("x-correlation-id");
  ASSERT_TRUE(id.has_value());
  EXPECT_FALSE(id->empty());
  // The body repeats the same id so a client report can be tied to the log line.
  EXPECT_NE(response.body.find(*id), std::string::npos);
  EXPECT_EQ(response.headers.Get("content-type").value_or(""), "application/json");
}

TEST(ServerDispatchTest, NonStdExceptionBecomesA500) {
  RequestHandler handler = [](const HttpRequest&) -> HttpResponse { throw 42; };
  const HttpResponse response = InvokeHandlerGuarded(handler, SampleRequest());
  EXPECT_EQ(response.status, 500);
  EXPECT_TRUE(response.headers.Get("x-correlation-id").has_value());
}

TEST(ServerDispatchTest, DistinctFailuresGetDistinctCorrelationIds) {
  RequestHandler handler = [](const HttpRequest&) -> HttpResponse {
    throw std::runtime_error("x");
  };
  const auto a = InvokeHandlerGuarded(handler, SampleRequest());
  const auto b = InvokeHandlerGuarded(handler, SampleRequest());
  EXPECT_NE(a.headers.Get("x-correlation-id"), b.headers.Get("x-correlation-id"));
}

TEST(ServerDispatchTest, EmptyHandlerIsA503NotACrash) {
  const HttpResponse response = InvokeHandlerGuarded(RequestHandler{}, SampleRequest());
  EXPECT_EQ(response.status, 503);
  EXPECT_FALSE(response.headers.Get("x-correlation-id").has_value());
}

}  // namespace
}  // namespace smithy::http
