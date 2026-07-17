#ifndef SMITHY_HTTP_BEAST_TRANSPORT_H_
#define SMITHY_HTTP_BEAST_TRANSPORT_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "smithy/http/transport.h"

namespace smithy {
// smithy/client/config.h — forward-declared so this header stays includable
// without the client headers. The library dependency is deliberate:
// FromConfig is the ClientConfig→transport bridge, and it lives here because
// only this side can construct a Beast client while :client stays Boost-free.
struct ClientConfig;
}  // namespace smithy

namespace smithy::http {

// Production HTTP/1.1 server transport on Boost.Beast/asio (ADR-0006):
// concurrent connections on an asio thread pool (bounded by
// Options::max_connections), handlers on their own executor
// (Options::handler_threads) so blocking handlers don't starve the wire,
// keep-alive, per-connection timeouts, graceful shutdown, and optional TLS
// termination (ADR-0007).
// This is what generated services should run on; WebSocket upgrades
// (Phase 8) extend this transport.
//
//   smithy::http::BeastServerTransport server({.port = 8080});
//   server.Start(service.Handler());
//   ...
//   server.Stop();
class BeastServerTransport : public HttpServerTransport {
 public:
  struct Options {
    // "127.0.0.1" keeps test servers private; use "0.0.0.0" to serve externally.
    std::string address = "127.0.0.1";
    int port = 0;  // 0 binds an ephemeral port; read port() after Start.
    int threads = 4;
    // Handlers run on this dedicated pool, so a blocked handler (DB call,
    // downstream RPC, lock) cannot starve the io threads that accept
    // connections and read/write the wire (issue #46) — size it for your
    // handlers' blocking profile. 0 runs handlers inline on the io pool,
    // saving the executor hop when every handler is CPU-cheap and
    // non-blocking.
    int handler_threads = 16;
    // Concurrent-connection cap: at the limit the server stops accepting and
    // new connections wait in the kernel's listen backlog until a session
    // closes, so a connection flood cannot exhaust fds/memory (issue #46).
    // Idle keep-alive sessions still expire on request_timeout_seconds, so
    // they cannot pin the cap forever. 0 means unlimited.
    std::size_t max_connections = 1024;
    int request_timeout_seconds = 30;
    // Over-limit requests are answered (413 for the body, 431 for headers)
    // with Connection: close and a bounded lingering close, not silently
    // aborted (issue #94).
    std::size_t max_body_bytes = std::size_t{64} * 1024 * 1024;
    std::size_t max_header_bytes = std::size_t{8} * 1024;
    // Stop() drains: no new connections or keep-alive reads, and in-flight
    // requests get this long to finish before the pool is torn down.
    int drain_timeout_seconds = 10;
    // TLS termination: set both (PEM text, not file paths) to serve https.
    std::string tls_certificate_chain_pem;
    std::string tls_private_key_pem;
  };

  BeastServerTransport() : BeastServerTransport(Options{}) {}
  explicit BeastServerTransport(Options options);
  ~BeastServerTransport() override;

  Outcome<Unit> Start(RequestHandler handler) override;
  void Stop() override;

  int port() const;

 private:
  struct State;  // Hides boost headers from this public header.

  void Shutdown() noexcept;

  Options options_;
  std::shared_ptr<State> state_;
  std::vector<std::thread> threads_;
};

// Production HTTP/1.1 client transport on Boost.Beast/asio (ADR-0007):
// keep-alive connection reuse behind a small idle pool, per-request
// timeouts, and TLS via asio SSL (BoringSSL) with certificate and hostname
// verification on by default. Thread-safe: concurrent Send() calls use
// distinct connections.
//
//   smithy::ClientConfig config;
//   config.endpoint = "https://api.example.com";
//   config.tls.ca_pem = corp_ca_pem;               // when not publicly trusted
//   auto transport = smithy::http::BeastHttpClient::FromConfig(config);
//   if (!transport) { /* bad endpoint */ }
//   config.http_client = *transport;               // the wire
//   auto client = MyServiceClient::Create(std::move(config));
class BeastHttpClient : public HttpClient {
 public:
  // Direct construction for tests and custom wiring. Production
  // configuration should flow through FromConfig so every knob lives on the
  // one ClientConfig (issue #49).
  struct Options {
    std::string host;
    int port = 80;
    bool tls = false;
    // Verification knobs when `tls` is true — the same struct ClientConfig
    // carries, so FromConfig copies it wholesale and the two can't drift.
    TlsOptions tls_options;
    int request_timeout_ms = 30000;
    // Idle keep-alive connections retained for reuse.
    std::size_t max_idle_connections = 4;
  };

  explicit BeastHttpClient(Options options);
  ~BeastHttpClient() override;

  // One-stop construction from the ClientConfig the generated client will
  // use: endpoint (scheme/host/port), tls.verify_peer/tls.ca_pem,
  // request_timeout_ms, and max_idle_connections all come from the config,
  // so nothing is configured twice. Fails on an unparsable or non-http(s)
  // endpoint. Any endpoint path prefix stays the generated client's job.
  static Outcome<std::shared_ptr<BeastHttpClient>> FromConfig(const ClientConfig& config);

  Outcome<HttpResponse> Send(const HttpRequest& request) override;

 private:
  struct State;  // Hides boost headers from this public header.

  std::shared_ptr<State> state_;
};

}  // namespace smithy::http

#endif  // SMITHY_HTTP_BEAST_TRANSPORT_H_
