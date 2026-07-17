#include "smithy/http/socket_transport.h"

#include <gtest/gtest.h>

#include <stdexcept>
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

TEST(SocketTransportTest, ThrowingHandlerBecomesA500NotACrash) {
  // The exception escapes the handler on the transport's own thread; before the
  // guard this unwound out of the accept loop and terminated the process. The
  // server must instead answer 500 and stay up for the next request.
  SocketHttpServer server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) -> HttpResponse {
                    if (request.target == "/boom") {
                      throw std::runtime_error("handler blew up");
                    }
                    return HttpResponse{200, {}, "ok"};
                  })
                  .ok());
  SocketHttpClient client("127.0.0.1", server.port());

  HttpRequest boom;
  boom.target = "/boom";
  const auto failed = client.Send(boom);
  ASSERT_TRUE(failed.ok()) << failed.error().message();
  EXPECT_EQ(failed->status, 500);
  EXPECT_FALSE(failed->headers.Get("x-correlation-id").value_or("").empty());

  // The server survived: a subsequent request still succeeds.
  HttpRequest fine;
  fine.target = "/ok";
  const auto ok = client.Send(fine);
  ASSERT_TRUE(ok.ok()) << ok.error().message();
  EXPECT_EQ(ok->status, 200);
  EXPECT_EQ(ok->body, "ok");

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

TEST(SocketTransportTest, StripsHandlerSetFramingHeaders) {
  // The transport is authoritative for framing (issue #46): it already strips
  // handler-set content-length/connection, but a handler-set
  // transfer-encoding next to the transport's own content-length is the
  // classic request-smuggling pair and must be dropped too.
  SocketHttpServer server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 200;
                    response.headers.Set("Content-Length", "999");
                    response.headers.Set("Transfer-Encoding", "chunked");
                    response.headers.Set("Connection", "keep-alive");
                    response.headers.Set("x-app", "kept");
                    response.body = "abc";
                    return response;
                  })
                  .ok());
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->body, "abc");
  EXPECT_FALSE(response->headers.Has("transfer-encoding"));
  EXPECT_EQ(response->headers.Get("content-length"), "3");
  EXPECT_EQ(response->headers.Get("connection"), "close");
  EXPECT_EQ(response->headers.Get("x-app"), "kept");
  server.Stop();
}

TEST(SocketTransportTest, PeerCloseMidSendIsAnErrorNotSigpipe) {
  SocketHttpServer server;
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{200, {}, "ok"}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());

  // Over the server's 64 MiB body cap: it rejects on content-length and
  // closes while the client is still writing. That must surface as a
  // transport error (or an HTTP error status), never a SIGPIPE that kills
  // the process.
  HttpRequest request;
  request.method = "POST";
  request.target = "/";
  request.body = std::string((std::size_t{64} << 20) + 1024, 'x');
  const auto response = client.Send(request);
  if (response.ok()) {
    EXPECT_GE(response->status, 400);
  } else {
    EXPECT_EQ(response.error().kind(), ErrorKind::kTransport);
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
