// BeastHttpClient against BeastServerTransport: plaintext round trips with
// keep-alive reuse, TLS with certificate + hostname verification against the
// server's TLS termination, the verification failure mode, and the server's
// TLS posture (version floor, ALPN).

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "smithy/client/config.h"
#include "smithy/http/beast_transport.h"
#include "smithy/http/message.h"
#include "smithy/http/trace_context.h"
#include "smithy/http/transport.h"
#include "smithy/testing/tls_test_identity.h"

namespace smithy::http {
namespace {

using smithy::testing::kTestCertificatePem;
using smithy::testing::kTestPrivateKeyPem;

RequestHandler EchoHandler() {
  return [](const HttpRequest& request) {
    HttpResponse response;
    response.status = 200;
    response.headers.Set("x-echo-method", request.method);
    response.headers.Set("x-echo-target", request.target);
    response.body = request.body;
    return response;
  };
}

HttpRequest PostRequest(const std::string& body) {
  HttpRequest request;
  request.method = "POST";
  request.target = "/echo";
  request.headers.Set("content-type", "text/plain");
  request.headers.Set("content-length", std::to_string(body.size()));
  request.body = body;
  return request;
}

// The TLS-terminating server every TLS test here dials.
BeastServerTransport::Options TlsServerOptions(int threads = 1) {
  return {.port = 0,
          .threads = threads,
          .tls_certificate_chain_pem = kTestCertificatePem,
          .tls_private_key_pem = kTestPrivateKeyPem};
}

TEST(BeastClientTest, PlaintextRoundTripsAndReusesConnections) {
  BeastServerTransport server({.port = 0, .threads = 2});
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  // Several sequential requests: the second and later ones ride the pooled
  // keep-alive connection.
  for (int i = 0; i < 3; ++i) {
    const std::string body = "hello " + std::to_string(i);
    auto response = client.Send(PostRequest(body));
    ASSERT_TRUE(response.ok()) << response.error().message();
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, body);
    EXPECT_EQ(response->headers.Get("x-echo-method").value_or(""), "POST");
    EXPECT_EQ(response->headers.Get("x-echo-target").value_or(""), "/echo");
  }
  server.Stop();
}

TEST(BeastClientTest, SurvivesServerRestartBetweenRequests) {
  // The pooled connection dies with the server; the client must notice the
  // stale connection and redial instead of failing the request.
  auto server = std::make_unique<BeastServerTransport>(
      BeastServerTransport::Options{.port = 0, .threads = 1});
  ASSERT_TRUE(server->Start(EchoHandler()).ok());
  const int port = server->port();

  BeastHttpClient client({.host = "127.0.0.1", .port = port});
  ASSERT_TRUE(client.Send(PostRequest("one")).ok());

  server->Stop();
  server = std::make_unique<BeastServerTransport>(
      BeastServerTransport::Options{.port = port, .threads = 1});
  ASSERT_TRUE(server->Start(EchoHandler()).ok());

  auto response = client.Send(PostRequest("two"));
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "two");
  server->Stop();
}

TEST(BeastClientTest, MintsDistinctTraceIdsAcrossKeepAliveRequests) {
  // ADR-0011 on the production transport: minting is per request, not per
  // connection — two requests riding one pooled keep-alive connection get
  // two identities.
  BeastServerTransport server({.port = 0, .threads = 1});
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    HttpResponse response;
                    response.headers.Set("x-trace",
                                         request.headers.Get("traceparent").value_or(""));
                    return response;
                  })
                  .ok());

  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  const auto first = client.Send(PostRequest("one"));
  const auto second = client.Send(PostRequest("two"));  // rides the pooled connection
  ASSERT_TRUE(first.ok() && second.ok());
  const auto first_trace = ParseTraceparent(first->headers.Get("x-trace").value_or(""));
  const auto second_trace = ParseTraceparent(second->headers.Get("x-trace").value_or(""));
  ASSERT_TRUE(first_trace.has_value() && second_trace.has_value());
  EXPECT_NE(first_trace->trace_id, second_trace->trace_id);
}

TEST(BeastClientTest, TlsRoundTripsWithCustomCa) {
  BeastServerTransport server(TlsServerOptions(/*threads=*/2));
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  BeastHttpClient client({.host = "127.0.0.1",
                          .port = server.port(),
                          .tls = true,
                          .tls_options = {.ca_pem = kTestCertificatePem}});
  for (int i = 0; i < 2; ++i) {
    auto response = client.Send(PostRequest("secret"));
    ASSERT_TRUE(response.ok()) << response.error().message();
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, "secret");
  }
  server.Stop();
}

TEST(BeastClientTest, TlsVerificationRejectsUntrustedServers) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  // Default trust roots do not contain the self-signed test certificate.
  BeastHttpClient client({.host = "127.0.0.1", .port = server.port(), .tls = true});
  auto response = client.Send(PostRequest("secret"));
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().kind(), ErrorKind::kTransport);
  server.Stop();
}

TEST(BeastClientTest, TlsVerificationCanBeDisabledExplicitly) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  BeastHttpClient client({.host = "127.0.0.1",
                          .port = server.port(),
                          .tls = true,
                          .tls_options = {.verify_peer = false}});
  auto response = client.Send(PostRequest("secret"));
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "secret");
  server.Stop();
}

TEST(BeastClientTest, FromConfigParsesTheEndpointAndRejectsBadSchemes) {
  ClientConfig config;
  config.endpoint = "https://api.example.com";
  ASSERT_TRUE(BeastHttpClient::FromConfig(config).ok());
  config.endpoint = "http://api.example.com:8080/prefix";
  ASSERT_TRUE(BeastHttpClient::FromConfig(config).ok());
  config.endpoint = "ftp://api.example.com";
  EXPECT_FALSE(BeastHttpClient::FromConfig(config).ok());
  config.endpoint = "";
  EXPECT_FALSE(BeastHttpClient::FromConfig(config).ok());
}

TEST(BeastClientTest, FromConfigHonorsTheConfigsTlsKnobs) {
  BeastServerTransport server(TlsServerOptions(/*threads=*/2));
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  // The one-stop production path (issue #49): endpoint, TLS trust, timeout,
  // and pool size all come from the one ClientConfig the generated client
  // will also use — nothing is configured twice.
  ClientConfig config;
  config.endpoint = "https://127.0.0.1:" + std::to_string(server.port());
  config.tls.ca_pem = kTestCertificatePem;
  auto client = BeastHttpClient::FromConfig(config);
  ASSERT_TRUE(client.ok()) << client.error().message();
  auto response = (*client)->Send(PostRequest("secret"));
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "secret");
  server.Stop();
}

TEST(BeastClientTest, ConfigAndOptionsDefaultsAgree) {
  // The two knobs FromConfig copies as scalars are defaulted in both structs
  // (the TLS knobs share one struct and can't drift); this guards the pair.
  const ClientConfig config;
  const BeastHttpClient::Options options;
  EXPECT_EQ(config.request_timeout_ms, options.request_timeout_ms);
  EXPECT_EQ(config.max_idle_connections, options.max_idle_connections);
}

TEST(BeastClientTest, RejectsTlsServerMisconfiguration) {
  BeastServerTransport server(
      {.port = 0, .tls_certificate_chain_pem = kTestCertificatePem});  // key missing
  EXPECT_FALSE(server.Start(EchoHandler()).ok());
}

// Raw TLS dialer for the posture tests: BeastHttpClient can't be talked into
// an old protocol version or a custom ALPN list, so these handshake with
// asio::ssl directly. No verification — the posture, not trust, is under
// test.
struct RawTlsProbe {
  boost::asio::io_context io;
  boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_client};
  std::optional<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> stream;

  // Returns the handshake's error code; success is a falsy code.
  boost::system::error_code Handshake(int port) {
    stream.emplace(io, ctx);
    boost::asio::ip::tcp::resolver resolver(io);
    boost::system::error_code ec;
    const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port), ec);
    if (!ec) {
      (void)boost::asio::connect(stream->next_layer(), endpoints, ec);
    }
    if (!ec) {
      (void)stream->handshake(boost::asio::ssl::stream_base::client, ec);
    }
    return ec;
  }
};

TEST(BeastClientTest, TlsServerRefusesPreTls12Clients) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  RawTlsProbe probe;
  ASSERT_EQ(SSL_CTX_set_max_proto_version(probe.ctx.native_handle(), TLS1_1_VERSION), 1);
  EXPECT_TRUE(probe.Handshake(server.port()));  // refused below the TLS 1.2 floor
  server.Stop();
}

TEST(BeastClientTest, Tls12CipherPolicyIsEcdheAeadOnly) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  // A TLS 1.2 client with default ciphers lands on an ECDHE+AEAD suite (1.3
  // is capped away so cipher_list, not the fixed 1.3 suites, decides).
  RawTlsProbe aead;
  ASSERT_EQ(SSL_CTX_set_max_proto_version(aead.ctx.native_handle(), TLS1_2_VERSION), 1);
  ASSERT_FALSE(aead.Handshake(server.port()));
  const std::string cipher =
      SSL_CIPHER_get_name(SSL_get_current_cipher(aead.stream->native_handle()));
  EXPECT_TRUE(cipher.find("GCM") != std::string::npos ||
              cipher.find("CHACHA20") != std::string::npos)
      << cipher;

  // A client that can only do CBC-mode 1.2 suites is refused. (The AES-SHA1
  // variants are the one CBC family BoringSSL still ships; the SHA384 CBC
  // suites OpenSSL keeps would make this assert fail to even set up there.)
  RawTlsProbe cbc;
  ASSERT_EQ(SSL_CTX_set_max_proto_version(cbc.ctx.native_handle(), TLS1_2_VERSION), 1);
  ASSERT_EQ(SSL_CTX_set_cipher_list(cbc.ctx.native_handle(),
                                    "ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES128-SHA"),
            1);
  EXPECT_TRUE(cbc.Handshake(server.port()));
  server.Stop();
}

TEST(BeastClientTest, TlsServerNegotiatesHttp11Alpn) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  RawTlsProbe probe;
  // Wire format: length-prefixed names. h2 first — selection must be by
  // support, not offer order. (set_alpn_protos returns 0 on success.)
  const unsigned char offer[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  ASSERT_EQ(SSL_CTX_set_alpn_protos(probe.ctx.native_handle(), offer, sizeof(offer)), 0);
  ASSERT_FALSE(probe.Handshake(server.port()));

  const unsigned char* selected = nullptr;
  unsigned int selected_len = 0;
  SSL_get0_alpn_selected(probe.stream->native_handle(), &selected, &selected_len);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(selected), selected_len), "http/1.1");
  server.Stop();
}

TEST(BeastClientTest, TlsServerRefusesAlpnWithoutHttp11) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  RawTlsProbe probe;
  const unsigned char offer[] = {2, 'h', '2'};
  ASSERT_EQ(SSL_CTX_set_alpn_protos(probe.ctx.native_handle(), offer, sizeof(offer)), 0);
  // An ALPN offer with no overlap fails the handshake (no_application_protocol)
  // instead of silently proceeding in a protocol the client didn't agree to.
  EXPECT_TRUE(probe.Handshake(server.port()));
  server.Stop();
}

}  // namespace
}  // namespace smithy::http
