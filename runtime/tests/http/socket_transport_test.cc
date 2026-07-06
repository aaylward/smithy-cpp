#include "smithy/http/socket_transport.h"

#include <gtest/gtest.h>

#include <string>

namespace smithy::http {
namespace {

TEST(SocketTransportTest, RoundTripsOverRealSockets) {
  SocketHttpServer server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    HttpResponse response;
                    response.status = 200;
                    response.headers.Set("content-type", "text/plain");
                    response.headers.Set("x-method", request.method);
                    response.headers.Set("x-target", request.target);
                    response.headers.Set("x-probe",
                                         request.headers.Get("x-probe").value_or("missing"));
                    response.body = "echo:" + request.body;
                    return response;
                  })
                  .ok());
  ASSERT_GT(server.port(), 0);

  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "POST";
  request.target = "/cities/a%20b?pageSize=10";
  request.headers.Set("x-probe", "42");
  request.body = "hello over tcp";

  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->headers.Get("x-method"), "POST");
  EXPECT_EQ(response->headers.Get("x-target"), "/cities/a%20b?pageSize=10");
  EXPECT_EQ(response->headers.Get("x-probe"), "42");
  EXPECT_EQ(response->body, "echo:hello over tcp");

  server.Stop();
}

TEST(SocketTransportTest, HandlesSequentialRequestsAndLargeBodies) {
  SocketHttpServer server;
  ASSERT_TRUE(
      server.Start([](const HttpRequest& request) { return HttpResponse{200, {}, request.body}; })
          .ok());
  SocketHttpClient client("127.0.0.1", server.port());

  const std::string large(1 << 20, 'x');  // 1 MiB
  for (int i = 0; i < 3; ++i) {
    HttpRequest request;
    request.method = "POST";
    request.target = "/echo";
    request.body = large;
    const auto response = client.Send(request);
    ASSERT_TRUE(response.ok()) << response.error().message();
    EXPECT_EQ(response->body.size(), large.size());
  }
  server.Stop();
}

TEST(SocketTransportTest, ReportsConnectionFailure) {
  SocketHttpServer throwaway;
  ASSERT_TRUE(throwaway.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  const int dead_port = throwaway.port();
  throwaway.Stop();  // port is now closed

  SocketHttpClient client("127.0.0.1", dead_port, /*timeout_ms=*/2000);
  const auto response = client.Send(HttpRequest{});
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().kind(), ErrorKind::kTransport);
}

TEST(SocketTransportTest, StopIsIdempotent) {
  SocketHttpServer server;
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  server.Stop();
  server.Stop();  // must not hang or crash
}

}  // namespace
}  // namespace smithy::http
