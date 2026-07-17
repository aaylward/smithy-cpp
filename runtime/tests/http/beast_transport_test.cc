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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "smithy/http/socket_transport.h"

namespace smithy::http {
namespace {

// Opens a loopback connection to `port` with a bounded receive timeout (and
// SIGPIPE disarmed where SO_NOSIGPIPE exists); returns the fd, or -1.
int ConnectLoopback(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  timeval timeout{.tv_sec = 10, .tv_usec = 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
  const int no_sigpipe = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Fans out `clients` concurrent SocketHttpClients, each POSTing /echo with a
// distinct body; returns how many did not get their body echoed back.
int FanOutEchoFailures(int port, int clients) {
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  threads.reserve(static_cast<std::size_t>(clients));
  for (int i = 0; i < clients; ++i) {
    threads.emplace_back([&failures, port, i] {
      SocketHttpClient client("127.0.0.1", port);
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
  for (std::thread& thread : threads) {
    thread.join();
  }
  return failures.load();
}

// Sends raw bytes and returns the raw response so assertions can see the
// exact wire framing (a parsing client would mask duplicate headers).
std::string RawRoundTrip(int port, const std::string& request_bytes) {
  const int fd = ConnectLoopback(port);
  if (fd < 0) return {};
  (void)::send(fd, request_bytes.data(), request_bytes.size(), 0);
  std::string received;
  char scratch[1024];
  for (;;) {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    if (n <= 0) break;
    received.append(scratch, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return received;
}

std::string AsciiLowerCopy(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

std::size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  for (auto pos = haystack.find(needle); pos != std::string::npos;
       pos = haystack.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

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

  EXPECT_EQ(FanOutEchoFailures(server.port(), 8), 0);
  server.Stop();
  EXPECT_GT(max_in_flight.load(), 1) << "requests were serialized";
}

TEST(BeastTransportTest, StripsHandlerSetFramingHeaders) {
  // The transport is authoritative for framing (issue #46): handler-set
  // framing headers must never reach the wire. A duplicate or conflicting
  // content-length / transfer-encoding pair is the classic request-smuggling
  // vector, and the connection token must state what the server actually
  // does. The socket server already strips these; this pins Beast to it.
  BeastServerTransport server;
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
  const std::string raw =
      RawRoundTrip(server.port(), "GET / HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  ASSERT_FALSE(raw.empty());
  const std::string wire = AsciiLowerCopy(raw);
  EXPECT_EQ(CountOccurrences(wire, "content-length:"), 1u) << raw;
  EXPECT_NE(wire.find("content-length: 3\r\n"), std::string::npos) << raw;
  EXPECT_EQ(wire.find("transfer-encoding"), std::string::npos) << raw;
  EXPECT_EQ(CountOccurrences(wire, "connection:"), 1u) << raw;
  EXPECT_EQ(wire.find("keep-alive"), std::string::npos) << raw;
  EXPECT_NE(wire.find("connection: close\r\n"), std::string::npos) << raw;
  EXPECT_NE(wire.find("x-app: kept\r\n"), std::string::npos) << raw;
  EXPECT_NE(wire.find("\r\n\r\nabc"), std::string::npos) << raw;
  server.Stop();
}

TEST(BeastTransportTest, MaxConnectionsBoundsConcurrencyWithoutRejecting) {
  // Issue #46: at the cap the server pauses accepting — new connections wait
  // in the kernel's listen backlog until a session closes — rather than
  // rejecting or allocating unbounded per-connection state. Every client
  // still gets an answer; with the cap of one, handlers serialize even
  // though four io threads are available (without the cap this measures >1,
  // as ServesConcurrentConnections proves).
  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  BeastServerTransport server(BeastServerTransport::Options{.threads = 4, .max_connections = 1});
  ASSERT_TRUE(server
                  .Start([&](const HttpRequest& request) {
                    const int now = ++in_flight;
                    int expected = max_in_flight.load();
                    while (now > expected && !max_in_flight.compare_exchange_weak(expected, now)) {
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    --in_flight;
                    return HttpResponse{200, {}, request.body};
                  })
                  .ok());

  EXPECT_EQ(FanOutEchoFailures(server.port(), 6), 0);
  server.Stop();
  EXPECT_EQ(max_in_flight.load(), 1) << "a cap of one connection must serialize handlers";
}

TEST(BeastTransportTest, IdleKeepAliveSessionCannotPinTheCap) {
  // The cap's one starvation hazard: an idle keep-alive session holds a slot
  // without doing work. The idle read must expire on request_timeout_seconds
  // and free the slot, or cap + one lazy client = permanent starvation (the
  // production guide promises this).
  BeastServerTransport server(BeastServerTransport::Options{
      .threads = 2, .max_connections = 1, .request_timeout_seconds = 1});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{200, {}, "ok"}; }).ok());

  // Session 1: keep-alive request, then hold the connection open, idle.
  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);
  const std::string head = "GET / HTTP/1.1\r\nhost: x\r\n\r\n";  // HTTP/1.1: keep-alive
  ASSERT_EQ(::send(fd, head.data(), head.size(), 0), static_cast<ssize_t>(head.size()));
  std::string received;
  char scratch[512];
  while (received.find("\r\n\r\nok") == std::string::npos) {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    ASSERT_GT(n, 0) << "session 1 never got its response";
    received.append(scratch, static_cast<std::size_t>(n));
  }

  // Session 2 waits in the backlog until session 1's idle read times out
  // (~1s), then must be served. If the timeout didn't free the slot, this
  // Send would hang until the client gives up.
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
  ::close(fd);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  server.Stop();
}

TEST(BeastTransportTest, ZeroMaxConnectionsMeansUnlimited) {
  // 0 disables the cap; a flipped comparison would turn it into "never
  // accept", which this round trip would catch as a hang/timeout.
  BeastServerTransport server(BeastServerTransport::Options{.max_connections = 0});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{204, {}, ""}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 204);
  server.Stop();
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
  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);

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

TEST(BeastTransportTest, BlockedHandlersDoNotStarveTheIoPool) {
  // Issue #46: handlers run on their own executor (Options::handler_threads),
  // so even a single io thread keeps accepting connections and reading
  // requests while several handlers block concurrently. With handlers inline
  // on the io pool, threads = 1 would serialize them and this barrier could
  // never fill — each handler would report "starved" after its bounded wait.
  constexpr int kConcurrent = 3;
  std::atomic<int> waiting{0};
  BeastServerTransport server(BeastServerTransport::Options{.threads = 1});
  ASSERT_TRUE(
      server
          .Start([&](const HttpRequest&) {
            ++waiting;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (waiting.load() < kConcurrent && std::chrono::steady_clock::now() < deadline) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const bool all_blocked_together = waiting.load() >= kConcurrent;
            return HttpResponse{200, {}, all_blocked_together ? "ok" : "starved"};
          })
          .ok());

  std::vector<std::thread> clients;
  std::atomic<int> ok{0};
  clients.reserve(kConcurrent);
  for (int i = 0; i < kConcurrent; ++i) {
    clients.emplace_back([&] {
      SocketHttpClient client("127.0.0.1", server.port());
      const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
      if (response.ok() && response->body == "ok") {
        ++ok;
      }
    });
  }
  for (std::thread& thread : clients) {
    thread.join();
  }
  server.Stop();
  EXPECT_EQ(ok.load(), kConcurrent) << "handlers serialized on the io pool";
}

TEST(BeastTransportTest, ThrowingHandlerBecomesA500NotACrash) {
  // The #41 guard must hold on the executor too: an exception escaping into
  // an asio::thread_pool worker would std::terminate the process. Exercise
  // both dispatch modes — the pool (default) and inline (handler_threads=0).
  for (const int handler_threads : {16, 0}) {
    BeastServerTransport server(BeastServerTransport::Options{.handler_threads = handler_threads});
    ASSERT_TRUE(server
                    .Start([](const HttpRequest& request) -> HttpResponse {
                      if (request.target == "/boom") {
                        throw std::runtime_error("handler bug");
                      }
                      return HttpResponse{200, {}, "fine"};
                    })
                    .ok());
    SocketHttpClient client("127.0.0.1", server.port());
    const auto boom = client.Send(HttpRequest{"GET", "/boom", {}, ""});
    ASSERT_TRUE(boom.ok()) << boom.error().message();
    EXPECT_EQ(boom->status, 500) << "handler_threads=" << handler_threads;
    EXPECT_FALSE(boom->headers.Get("x-correlation-id").value_or("").empty());
    // The process and the server both survived: the next request serves.
    SocketHttpClient again("127.0.0.1", server.port());
    const auto ok = again.Send(HttpRequest{"GET", "/", {}, ""});
    ASSERT_TRUE(ok.ok()) << ok.error().message();
    EXPECT_EQ(ok->status, 200);
    server.Stop();
  }
}

TEST(BeastTransportTest, KeepAliveConnectionServesSequentialRequestsViaTheExecutor) {
  // Pins the Respond → ReadNext re-arm across the executor hop: two requests
  // on one keep-alive connection, both answered, in order.
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    return HttpResponse{200, {}, "echo:" + request.body};
                  })
                  .ok());
  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);
  std::string received;
  char scratch[512];
  for (const std::string body : {"one", "two"}) {
    const std::string request =
        "POST / HTTP/1.1\r\nhost: x\r\ncontent-length: " + std::to_string(body.size()) +
        "\r\n\r\n" + body;
    ASSERT_EQ(::send(fd, request.data(), request.size(), 0), static_cast<ssize_t>(request.size()));
    const std::string expected = "echo:" + body;
    while (received.find(expected) == std::string::npos) {
      const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
      ASSERT_GT(n, 0) << "no response for request body '" << body << "'";
      received.append(scratch, static_cast<std::size_t>(n));
    }
  }
  ::close(fd);
  EXPECT_LT(received.find("echo:one"), received.find("echo:two"));
  server.Stop();
}

TEST(BeastTransportTest, BurstBeyondHandlerPoolQueuesAndCompletes) {
  // More concurrent requests than handler threads: the excess queues on the
  // executor and every client is still answered — no deadlock, no rejection.
  BeastServerTransport server(BeastServerTransport::Options{.threads = 2, .handler_threads = 2});
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    return HttpResponse{200, {}, request.body};
                  })
                  .ok());
  EXPECT_EQ(FanOutEchoFailures(server.port(), 6), 0);
  server.Stop();
}

TEST(BeastTransportTest, InlineHandlersStillServeWhenPoolDisabled) {
  // handler_threads = 0 opts back into inline dispatch on the io pool (no
  // executor hop) — the round trip must still work end to end.
  BeastServerTransport server(BeastServerTransport::Options{.handler_threads = 0});
  ASSERT_TRUE(
      server.Start([](const HttpRequest& request) { return HttpResponse{200, {}, request.body}; })
          .ok());
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"POST", "/", {}, "inline"});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "inline");
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

TEST(BeastTransportTest, StopWithWedgedHandlerReturnsWithinTheGrace) {
  // Issue #46's last drain gap: a handler that never returns must not wedge
  // Stop() forever. Past drain_timeout plus a short teardown grace the
  // transport abandons the stuck worker (deliberately leaking it — a thread
  // cannot be killed safely) and Stop() returns. Exercised in both dispatch
  // modes: a wedged pool worker and a wedged io thread behave the same.
  for (const int handler_threads : {16, 0}) {
    // Heap-allocated and captured by value: the abandoned handler thread may
    // outlive this loop iteration, so it must own its flags rather than
    // reference reused stack slots.
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto entered = std::make_shared<std::atomic<bool>>(false);
    BeastServerTransport server(BeastServerTransport::Options{.handler_threads = handler_threads,
                                                              .drain_timeout_seconds = 0});
    ASSERT_TRUE(server
                    .Start([release, entered](const HttpRequest&) {
                      entered->store(true);
                      while (!release->load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                      }
                      return HttpResponse{200, {}, ""};
                    })
                    .ok());
    std::thread caller([port = server.port()] {
      SocketHttpClient client("127.0.0.1", port, /*timeout_ms=*/2000);
      (void)client.Send(HttpRequest{"GET", "/", {}, ""});  // times out; expected
    });
    while (!entered->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto begin = std::chrono::steady_clock::now();
    server.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    EXPECT_LT(elapsed, std::chrono::seconds(10))
        << "Stop() wedged on a stuck handler (handler_threads=" << handler_threads << ")";

    release->store(true);  // unwedge: the abandoned reaper finishes the cleanup
    caller.join();
    // Give the (unobservable, by design) detached reaper a beat to complete
    // its cleanup so leak checkers don't sample the deliberate leak mid-heal.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
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
