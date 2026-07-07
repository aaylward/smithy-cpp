#include "smithy/http/beast_transport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "smithy/http/socket_transport.h"

namespace smithy::http {
namespace {

TEST(BeastTransportTest, RoundTripsOverRealSockets) {
  BeastServerTransport server;
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
  request.body = "hello beast";

  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->headers.Get("x-method"), "POST");
  EXPECT_EQ(response->headers.Get("x-target"), "/cities/a%20b?pageSize=10");
  EXPECT_EQ(response->headers.Get("x-probe"), "42");
  EXPECT_EQ(response->body, "echo:hello beast");

  server.Stop();
}

TEST(BeastTransportTest, ServesConcurrentConnections) {
  // The ADR-0005 transport handled one connection at a time; this is the
  // regression test that Beast genuinely serves in parallel.
  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  BeastServerTransport server(BeastServerTransport::Options{.threads = 4});
  ASSERT_TRUE(server
                  .Start([&](const HttpRequest& request) {
                    const int now = ++in_flight;
                    int expected = max_in_flight.load();
                    while (now > expected && !max_in_flight.compare_exchange_weak(expected, now)) {
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    --in_flight;
                    return HttpResponse{200, {}, request.body};
                  })
                  .ok());

  constexpr int kClients = 8;
  std::vector<std::thread> clients;
  std::atomic<int> failures{0};
  clients.reserve(kClients);
  for (int i = 0; i < kClients; ++i) {
    clients.emplace_back([&, i] {
      SocketHttpClient client("127.0.0.1", server.port());
      HttpRequest request;
      request.method = "POST";
      request.target = "/echo";
      request.body = "client-" + std::to_string(i);
      const auto response = client.Send(request);
      if (!response.ok() || response->body != request.body) {
        ++failures;
      }
    });
  }
  for (std::thread& thread : clients) {
    thread.join();
  }
  server.Stop();
  EXPECT_EQ(failures, 0);
  EXPECT_GT(max_in_flight.load(), 1) << "requests were serialized";
}

TEST(BeastTransportTest, HandlesLargeBodies) {
  BeastServerTransport server;
  ASSERT_TRUE(
      server.Start([](const HttpRequest& request) { return HttpResponse{200, {}, request.body}; })
          .ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "POST";
  request.target = "/echo";
  request.body = std::string(4 << 20, 'x');  // 4 MiB
  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body.size(), request.body.size());
  server.Stop();
}

TEST(BeastTransportTest, RejectsOversizedBodies) {
  BeastServerTransport server(BeastServerTransport::Options{.max_body_bytes = 1024});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "POST";
  request.target = "/";
  request.body = std::string(64 * 1024, 'x');
  const auto response = client.Send(request);
  // Beast closes the connection on a body-limit violation; either a transport
  // error or an HTTP error status is acceptable, but never a 200.
  if (response.ok()) {
    EXPECT_GE(response->status, 400);
  }
  server.Stop();
}

TEST(BeastTransportTest, RejectsOversizedHeaders) {
  BeastServerTransport server(BeastServerTransport::Options{.max_header_bytes = 1024});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{200, {}, ""}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "GET";
  request.target = "/";
  request.headers.Set("x-huge", std::string(8 * 1024, 'h'));
  const auto response = client.Send(request);
  // Beast closes the connection on a header-limit violation; either a
  // transport error or an HTTP error status is acceptable, but never a 200.
  if (response.ok()) {
    EXPECT_GE(response->status, 400);
  }
  server.Stop();
}

TEST(BeastTransportTest, StopDrainsInFlightRequests) {
  std::atomic<bool> handler_entered{false};
  BeastServerTransport server(BeastServerTransport::Options{.drain_timeout_seconds = 5});
  ASSERT_TRUE(server
                  .Start([&](const HttpRequest&) {
                    handler_entered = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    return HttpResponse{200, {}, "drained"};
                  })
                  .ok());

  const int port = server.port();
  Outcome<HttpResponse> response = HttpResponse{};
  std::thread caller([&] {
    SocketHttpClient client("127.0.0.1", port);
    response = client.Send(HttpRequest{"GET", "/", {}, ""});
  });
  while (!handler_entered) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Stop while the handler is mid-request: the response must still be
  // written in full before the pool is torn down.
  server.Stop();
  caller.join();
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->body, "drained");
}

TEST(BeastTransportTest, StartupErrorsAreReported) {
  BeastServerTransport bad(BeastServerTransport::Options{.address = "not-an-address"});
  EXPECT_FALSE(bad.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());

  BeastServerTransport first;
  ASSERT_TRUE(first.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  BeastServerTransport conflict(BeastServerTransport::Options{.port = first.port()});
  EXPECT_FALSE(conflict.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  first.Stop();
}

TEST(BeastTransportTest, StopIsIdempotentAndRestartable) {
  BeastServerTransport server;
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{204, {}, ""}; }).ok());
  server.Stop();
  server.Stop();
  // A stopped transport can be started again (fresh state).
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{205, {}, ""}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 205);
  server.Stop();
}

}  // namespace
}  // namespace smithy::http
