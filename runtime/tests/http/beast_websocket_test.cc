// Pins ADR-0015's WebSocket transports end to end over real loopback
// sockets: the upgrade path (gate refusals, handshake failures, plain-HTTP
// coexistence), the session contract (full duplex, backpressure,
// close-vs-error surfaces, protocol violations), TLS, and Stop() semantics
// with serve callbacks blocked mid-Receive.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "smithy/http/beast_transport.h"
#include "smithy/http/websocket.h"
#include "smithy/testing/connection_event_recorder.h"
#include "smithy/testing/tls_test_identity.h"

namespace smithy::http {
namespace {

using eventstream::Message;
using smithy::testing::ConnectionEventRecorder;

Message Text(const std::string& kind, const std::string& body) {
  return Message{.headers = {{":event-type", kind}}, .payload = Blob::FromString(body)};
}

HttpResponse PlainResponse(int status, const std::string& body) {
  HttpResponse response;
  response.status = status;
  response.body = body;
  return response;
}

// A server whose serve loop echoes every message back with an "echo:"
// prefix on the payload, then closes when the client does.
BeastServerTransport::Options EchoOptions() {
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) {
        return;
      }
      Message reply = **message;
      reply.payload = Blob::FromString("echo:" + (*message)->payload.ToString());
      if (!socket.Send(reply).ok()) {
        return;
      }
    }
  };
  return options;
}

RequestHandler NotFoundHandler() {
  return [](const HttpRequest&) { return PlainResponse(404, "no such route"); };
}

TEST(BeastWebSocketTest, MessagesRoundTripBothWaysOverTheUpgrade) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  for (int i = 0; i < 10; ++i) {
    const Message sent = Text("chat", "message-" + std::to_string(i));
    ASSERT_TRUE(socket->Send(sent).ok());
    auto received = socket->Receive();
    ASSERT_TRUE(received.ok()) << received.error().message();
    ASSERT_TRUE(received->has_value());
    EXPECT_EQ((**received).headers, sent.headers);
    EXPECT_EQ((**received).payload.ToString(), "echo:message-" + std::to_string(i));
  }

  // The client's close surfaces server-side as Receive's nullopt (the echo
  // loop returns), and the acknowledging close ends the client cleanly.
  socket->Close();
  auto after_close = socket->Receive();
  ASSERT_TRUE(after_close.ok()) << after_close.error().message();
  EXPECT_FALSE(after_close->has_value());
  server.Stop();
}

TEST(BeastWebSocketTest, ServerInitiatedCloseSurfacesAsNulloptClientSide) {
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    (void)socket.Send(Text("hello", "one message, then goodbye"));
    // Returning ends the session with a close handshake.
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  auto first = (*dialed)->Receive();
  ASSERT_TRUE(first.ok() && first->has_value());
  EXPECT_EQ((**first).payload.ToString(), "one message, then goodbye");
  auto second = (*dialed)->Receive();
  ASSERT_TRUE(second.ok()) << second.error().message();
  EXPECT_FALSE(second->has_value());
  server.Stop();
}

TEST(BeastWebSocketTest, FullDuplexSendsAndReceivesConcurrently) {
  // The server pushes its own stream while echoing nothing; the client
  // sends concurrently from a second thread. Neither direction may wait
  // for the other.
  constexpr int kEach = 50;
  std::atomic<int> server_received{0};
  BeastServerTransport::Options options;
  options.on_websocket = [&server_received](const HttpRequest&, WebSocket& socket) {
    std::thread pusher([&socket] {
      for (int i = 0; i < kEach; ++i) {
        if (!socket.Send(Text("push", "server-" + std::to_string(i))).ok()) {
          return;
        }
      }
    });
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) {
        break;
      }
      ++server_received;
    }
    pusher.join();
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  std::thread sender([&socket] {
    for (int i = 0; i < kEach; ++i) {
      ASSERT_TRUE(socket->Send(Text("send", "client-" + std::to_string(i))).ok());
    }
  });
  int client_received = 0;
  while (client_received < kEach) {
    auto message = socket->Receive();
    ASSERT_TRUE(message.ok() && message->has_value());
    ++client_received;
  }
  sender.join();
  socket->Close();
  auto end = socket->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
  EXPECT_EQ(client_received, kEach);
  // The server's receive loop sees every client message before the close.
  for (int i = 0; i < 200 && server_received.load() < kEach; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(server_received.load(), kEach);
  server.Stop();
}

TEST(BeastWebSocketTest, AReceiverThatDrainsLateStillGetsEveryMessageInOrder) {
  // Push well past the internal receive-queue depth while the client sits,
  // then drain: backpressure pauses the wire, resumes it, and loses
  // nothing.
  constexpr int kBurst = 40;
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    for (int i = 0; i < kBurst; ++i) {
      if (!socket.Send(Text("burst", std::to_string(i))).ok()) {
        return;
      }
    }
    // Hold the session open until the peer leaves.
    (void)socket.Receive();
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the burst pile up
  for (int i = 0; i < kBurst; ++i) {
    auto message = (*dialed)->Receive();
    ASSERT_TRUE(message.ok() && message->has_value()) << "message " << i;
    EXPECT_EQ((**message).payload.ToString(), std::to_string(i));
  }
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, TheGateRefusesBeforeAnyUpgradeExists) {
  BeastServerTransport::Options options = EchoOptions();
  options.websocket_gate = [](const HttpRequest& request) -> std::optional<HttpResponse> {
    if (request.headers.Get("authorization").value_or("") != "Bearer let-me-in") {
      return PlainResponse(401, "who are you?");
    }
    return std::nullopt;
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  // No credentials: the dial fails — the server answered 401, never 101.
  auto refused = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  EXPECT_FALSE(refused.ok());

  // Credentials ride the upgrade request's headers and the gate sees them.
  Headers credentials;
  credentials.Add("authorization", "Bearer let-me-in");
  auto admitted = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = server.port(), .headers = credentials});
  ASSERT_TRUE(admitted.ok()) << admitted.error().message();
  ASSERT_TRUE((*admitted)->Send(Text("chat", "hi")).ok());
  auto echo = (*admitted)->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:hi");
  server.Stop();
}

TEST(BeastWebSocketTest, PlainHttpRequestsStillServeNextToTheUpgradePath) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    return PlainResponse(200, "plain:" + request.target);
                  })
                  .ok());
  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  HttpRequest request;
  request.method = "GET";
  request.target = "/health";
  auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->body, "plain:/health");
  server.Stop();
}

TEST(BeastWebSocketTest, WithoutAServeCallbackUpgradesFlowToTheHttpHandler) {
  std::atomic<bool> handler_saw_upgrade{false};
  BeastServerTransport server(BeastServerTransport::Options{});  // no on_websocket
  ASSERT_TRUE(server
                  .Start([&handler_saw_upgrade](const HttpRequest& request) {
                    if (request.headers.Get("upgrade").has_value()) {
                      handler_saw_upgrade = true;
                      return PlainResponse(426, "upgrade not served here");
                    }
                    return PlainResponse(200, "ok");
                  })
                  .ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  EXPECT_FALSE(dialed.ok());  // the 426 is not a 101
  EXPECT_TRUE(handler_saw_upgrade.load());
  server.Stop();
}

// Sends raw bytes and returns whatever the server answers (bounded read).
std::string RawExchange(int port, const std::string& bytes) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  timeval timeout{.tv_sec = 5, .tv_usec = 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  std::string response;
  if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1 &&
      ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
      ::send(fd, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size())) {
    char scratch[2048];
    while (true) {
      const ssize_t n = ::recv(fd, scratch, sizeof(scratch), 0);
      if (n <= 0) break;
      response.append(scratch, static_cast<std::size_t>(n));
      if (response.find("\r\n\r\n") != std::string::npos) break;
    }
  }
  ::close(fd);
  return response;
}

TEST(BeastWebSocketTest, AFailedUpgradeHandshakeIsObservedAsUpgradeFailure) {
  ConnectionEventRecorder recorder;
  BeastServerTransport::Options options = EchoOptions();
  options.on_connection_event = recorder.Hook();
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  // An upgrade-shaped request without Sec-WebSocket-Key: is_upgrade
  // matches, the gate admits, the handshake then refuses.
  (void)RawExchange(server.port(),
                    "GET /stream HTTP/1.1\r\n"
                    "Host: test\r\n"
                    "Connection: Upgrade\r\n"
                    "Upgrade: websocket\r\n"
                    "\r\n");
  using Kind = BeastServerTransport::ConnectionEvent::Kind;
  ASSERT_TRUE(recorder.WaitFor(1));
  {
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    ASSERT_EQ(recorder.events.size(), 1u);
    EXPECT_EQ(recorder.events[0].kind, Kind::kUpgradeFailure);
    EXPECT_FALSE(recorder.events[0].peer_address.empty());
  }
  server.Stop();
}

// A hostile peer speaking raw Beast websocket, for protocol-violation
// tests the typed client cannot produce.
class RawWsPeer {
 public:
  explicit RawWsPeer(int port) : ws_(io_) {
    boost::asio::ip::tcp::resolver resolver(io_);
    boost::asio::connect(ws_.next_layer(), resolver.resolve("127.0.0.1", std::to_string(port)));
    ws_.handshake("127.0.0.1", "/");
  }
  void SendText(const std::string& text) {
    ws_.text(true);
    ws_.write(boost::asio::buffer(text));
  }
  void SendBinary(const std::string& bytes) {
    ws_.binary(true);
    ws_.write(boost::asio::buffer(bytes));
  }
  // Reads until close/error; true when the server closed the connection.
  bool DrainToClose() {
    boost::beast::flat_buffer buffer;
    boost::beast::error_code ec;
    while (!ec) {
      ws_.read(buffer, ec);
      buffer.consume(buffer.size());
    }
    return ec == boost::beast::websocket::error::closed || !!ec;
  }

 private:
  boost::asio::io_context io_;
  boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_;
};

TEST(BeastWebSocketTest, ATextMessageFailsTheSessionAsAProtocolError) {
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port());
  peer.SendText("this wire speaks event-stream frames, not text");
  auto result = serve_saw.get_future().get();
  ASSERT_FALSE(result.ok());  // an error, never nullopt: the peer misbehaved
  EXPECT_NE(result.error().message().find("text message"), std::string::npos);
  EXPECT_TRUE(peer.DrainToClose());  // and the peer was told, then dropped
  server.Stop();
}

TEST(BeastWebSocketTest, ABinaryMessageThatIsNotOneFrameFailsTheSession) {
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port());
  peer.SendBinary("garbage that is not an event-stream frame");
  auto result = serve_saw.get_future().get();
  ASSERT_FALSE(result.ok());
  server.Stop();
}

TEST(BeastWebSocketTest, ATrailingByteAfterTheFrameFailsTheSession) {
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto frame = eventstream::EncodeMessage(Text("chat", "valid"));
  ASSERT_TRUE(frame.ok());
  RawWsPeer peer(server.port());
  peer.SendBinary(*frame + std::string(1, '\0'));  // one frame plus one byte
  auto result = serve_saw.get_future().get();
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("exactly one"), std::string::npos);
  server.Stop();
}

TEST(BeastWebSocketTest, SendRefusesWhatTheCodecRefusesWithoutTouchingTheWire) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();

  Message unencodable;
  unencodable.headers.push_back({"", true});  // empty names cannot encode
  const auto refused = (*dialed)->Send(unencodable);
  ASSERT_FALSE(refused.ok());
  // The session is untouched: a valid message still round-trips.
  ASSERT_TRUE((*dialed)->Send(Text("chat", "still alive")).ok());
  auto echo = (*dialed)->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:still alive");
  server.Stop();
}

TEST(BeastWebSocketTest, StopUnblocksAServeCallbackMidReceive) {
  std::promise<Outcome<std::optional<Message>>> serve_result;
  BeastServerTransport::Options options;
  options.drain_timeout_seconds = 1;
  options.on_websocket = [&serve_result](const HttpRequest&, WebSocket& socket) {
    serve_result.set_value(socket.Receive());  // blocks until Stop aborts it
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let serve block

  const auto stop_started = std::chrono::steady_clock::now();
  server.Stop();
  const auto stop_took = std::chrono::steady_clock::now() - stop_started;
  EXPECT_LT(stop_took, std::chrono::seconds(8)) << "Stop() must not wait for stream sessions";

  auto result = serve_result.get_future().get();
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("server stopping"), std::string::npos);
}

TEST(BeastWebSocketTest, TlsStreamsCarryMessagesEndToEnd) {
  BeastServerTransport::Options options = EchoOptions();
  options.tls_certificate_chain_pem = smithy::testing::kTestCertificatePem;
  options.tls_private_key_pem = smithy::testing::kTestPrivateKeyPem;
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  BeastWebSocketClient::Options dial;
  dial.host = "127.0.0.1";
  dial.port = server.port();
  dial.tls = true;
  dial.tls_options.ca_pem = smithy::testing::kTestCertificatePem;
  auto dialed = BeastWebSocketClient::Dial(dial);
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();

  ASSERT_TRUE((*dialed)->Send(Text("secure", "over wss")).ok());
  auto echo = (*dialed)->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:over wss");
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, ConcurrentSessionsServeIndependently) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  constexpr int kClients = 4;
  std::atomic<int> failures{0};
  std::vector<std::thread> clients;
  clients.reserve(kClients);
  for (int c = 0; c < kClients; ++c) {
    clients.emplace_back([&failures, c, port = server.port()] {
      auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = port});
      if (!dialed.ok()) {
        ++failures;
        return;
      }
      for (int i = 0; i < 20; ++i) {
        const std::string body = "client-" + std::to_string(c) + "-" + std::to_string(i);
        if (!(*dialed)->Send(Text("chat", body)).ok()) {
          ++failures;
          return;
        }
        auto echo = (*dialed)->Receive();
        if (!echo.ok() || !echo->has_value() || (**echo).payload.ToString() != "echo:" + body) {
          ++failures;
          return;
        }
      }
      (*dialed)->Close();
    });
  }
  for (std::thread& client : clients) {
    client.join();
  }
  EXPECT_EQ(failures.load(), 0);
  server.Stop();
}

TEST(BeastWebSocketTest, DialFailuresAreErrorsNotCrashes) {
  // A port nobody listens on.
  const auto no_listener =
      BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = 1, .handshake_timeout_ms = 2000});
  EXPECT_FALSE(no_listener.ok());
  // A host that does not resolve.
  const auto no_host = BeastWebSocketClient::Dial(
      {.host = "no-such-host.invalid", .port = 80, .handshake_timeout_ms = 2000});
  EXPECT_FALSE(no_host.ok());
  // No host at all.
  EXPECT_FALSE(BeastWebSocketClient::Dial({}).ok());
}

TEST(BeastWebSocketTest, StartRefusesAServeCallbackWithoutAHandlerPool) {
  BeastServerTransport::Options options = EchoOptions();
  options.handler_threads = 0;
  BeastServerTransport server(options);
  const auto started = server.Start(NotFoundHandler());
  ASSERT_FALSE(started.ok());
  EXPECT_NE(started.error().message().find("handler_threads"), std::string::npos);
}

TEST(BeastWebSocketTest, StartRefusesAGateWithoutAServeCallback) {
  // A gate alone would be silently dead config: the upgrade path only
  // exists with on_websocket set.
  BeastServerTransport::Options options;
  options.websocket_gate = [](const HttpRequest&) -> std::optional<HttpResponse> {
    return std::nullopt;
  };
  BeastServerTransport server(options);
  const auto started = server.Start(NotFoundHandler());
  ASSERT_FALSE(started.ok());
  EXPECT_NE(started.error().message().find("on_websocket"), std::string::npos);
}

TEST(BeastWebSocketTest, DialWithPortZeroUsesTheSchemeDefault) {
  // tls=false + port 0 dials 80; nothing listens there in this test
  // environment, so the dial errors — but the error must name port 80,
  // proving the scheme default resolved (a wss dial defaulting to 80 was
  // the reviewed footgun).
  const auto dialed =
      BeastWebSocketClient::Dial({.host = "127.0.0.1", .handshake_timeout_ms = 2000});
  ASSERT_FALSE(dialed.ok());
  EXPECT_NE(dialed.error().message().find(":80"), std::string::npos) << dialed.error().message();
}

}  // namespace
}  // namespace smithy::http
