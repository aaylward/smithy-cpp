#include "smithy/http/beast_transport.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
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

TEST(BeastTransportTest, OversizedDeclaredBodyReadsA413) {
  BeastServerTransport server(BeastServerTransport::Options{.max_body_bytes = 1024});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "POST";
  request.target = "/";
  request.body = std::string(64 * 1024, 'x');
  const auto response = client.Send(request);
  // Issue #94: a declared Content-Length over the limit is the deterministic,
  // cheap-to-reject case — the parser fails at end-of-headers and the client
  // must reliably read a 413 (not a connection abort). The bounded lingering
  // close is what keeps the response readable while the client finishes
  // writing.
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 413);
  EXPECT_EQ(response->headers.Get("connection"), "close");
  server.Stop();
}

TEST(BeastTransportTest, OversizedHeadersReadA431) {
  BeastServerTransport server(BeastServerTransport::Options{.max_header_bytes = 1024});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{200, {}, ""}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "GET";
  request.target = "/";
  request.headers.Set("x-huge", std::string(8 * 1024, 'h'));
  const auto response = client.Send(request);
  // Issue #94: header-limit violations answer 431 before the close.
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 431);
  EXPECT_EQ(response->headers.Get("connection"), "close");
  server.Stop();
}

TEST(BeastTransportTest, MidStreamOverflowAnswersOrClosesButNeverHangs) {
  BeastServerTransport server(BeastServerTransport::Options{.max_body_bytes = 1024});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());

  // Raw chunked upload with no declared length: the parser only discovers the
  // overflow mid-body. The client must end up with a 413 or a closed
  // connection within the drain budget — never a hang (issue #94).
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
#ifdef SO_NOSIGPIPE
  int set = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
#endif
  timeval timeout{.tv_sec = 10, .tv_usec = 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(server.port()));
  ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif
  const std::string head = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
  (void)::send(fd, head.data(), head.size(), kSendFlags);
  const std::string chunk = "400\r\n" + std::string(1024, 'x') + "\r\n";  // 1 KiB per chunk
  for (int i = 0; i < 16; ++i) {  // 16 KiB total, well over the 1 KiB limit
    if (::send(fd, chunk.data(), chunk.size(), kSendFlags) < 0) {
      break;  // server already closed: acceptable, as long as we don't hang
    }
  }

  std::string received;
  char scratch[512];
  for (;;) {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    if (n <= 0) break;  // EOF, reset, or SO_RCVTIMEO — all bounded
    received.append(scratch, static_cast<std::size_t>(n));
  }
  ::close(fd);
  if (!received.empty()) {
    EXPECT_EQ(received.rfind("HTTP/1.1 413", 0), 0u) << received.substr(0, 64);
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
