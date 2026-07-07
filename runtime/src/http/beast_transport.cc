#include "smithy/http/beast_transport.h"

#include <algorithm>
#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "smithy/http/uri.h"

namespace smithy::http {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = boost::beast::http;

HttpRequest ToSmithyRequest(const bhttp::request<bhttp::string_body>& wire) {
  HttpRequest request;
  request.method = std::string(wire.method_string());
  request.target = std::string(wire.target());
  for (const auto& field : wire) {
    request.headers.Add(std::string(field.name_string()), std::string(field.value()));
  }
  request.body = wire.body();
  return request;
}

bhttp::response<bhttp::string_body> ToWireResponse(const HttpResponse& response, bool keep_alive) {
  bhttp::response<bhttp::string_body> wire;
  wire.result(static_cast<unsigned>(response.status));
  wire.version(11);
  for (const auto& [name, value] : response.headers.entries()) {
    wire.insert(name, value);
  }
  wire.body() = response.body;
  wire.keep_alive(keep_alive);
  wire.prepare_payload();  // sets content-length
  return wire;
}

// Half-closes the connection under any stream type (plain or TLS; TLS skips
// the close_notify exchange, which peers must tolerate per HTTP practice).
template <typename Stream>
void CloseStream(Stream& stream) {
  beast::error_code ignored;
  (void)beast::get_lowest_layer(stream).socket().shutdown(asio::ip::tcp::socket::shutdown_send,
                                                          ignored);
}

}  // namespace

// Shared by the acceptor loop, sessions, and the transport object; sessions
// may outlive Stop() briefly, so everything they touch lives here.
struct BeastServerTransport::State : std::enable_shared_from_this<State> {
  explicit State(const Options& options)
      : io(options.threads), acceptor(asio::make_strand(io)), opts(options) {}

  asio::io_context io;
  asio::ip::tcp::acceptor acceptor;
  Options opts;
  RequestHandler handler;
  // Engaged when TLS termination is configured (Start validated cert + key).
  std::optional<asio::ssl::context> ssl;
  std::atomic<bool> stopping{false};
  // Requests read off the wire whose responses are not fully written yet;
  // Stop() drains until this reaches zero (or the drain deadline passes).
  std::atomic<int> active{0};

  // All completion handlers capture State weakly: handlers queued inside the
  // io_context must not own the State that owns the io_context, or abandoned
  // handlers at shutdown would form a reference cycle and leak everything.

  void Accept() {
    acceptor.async_accept(
        asio::make_strand(io),
        [weak = weak_from_this()](beast::error_code ec, asio::ip::tcp::socket socket) {
          auto self = weak.lock();
          if (self == nullptr || ec || self->stopping) {
            return;  // Acceptor closed or shutting down.
          }
          self->RunSession(std::move(socket));
          self->Accept();
        });
  }

  void RunSession(asio::ip::tcp::socket socket) {
    if (!ssl.has_value()) {
      auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
      ReadNext(stream);
      return;
    }
    auto stream = std::make_shared<asio::ssl::stream<beast::tcp_stream>>(std::move(socket), *ssl);
    beast::get_lowest_layer(*stream).expires_after(
        std::chrono::seconds(opts.request_timeout_seconds));
    auto& stream_ref = *stream;
    stream_ref.async_handshake(asio::ssl::stream_base::server,
                               [weak = weak_from_this(), stream](beast::error_code ec) {
                                 auto self = weak.lock();
                                 if (self == nullptr || ec) {
                                   return;  // Handshake failure: drop the connection.
                                 }
                                 self->ReadNext(stream);
                               });
  }

  template <typename Stream>
  void ReadNext(const std::shared_ptr<Stream>& stream) {
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto parser = std::make_shared<bhttp::request_parser<bhttp::string_body>>();
    parser->body_limit(opts.max_body_bytes);
    parser->header_limit(static_cast<std::uint32_t>(opts.max_header_bytes));
    beast::get_lowest_layer(*stream).expires_after(
        std::chrono::seconds(opts.request_timeout_seconds));
    auto& stream_ref = *stream;
    bhttp::async_read(
        stream_ref, *buffer, *parser,
        [weak = weak_from_this(), stream, buffer, parser](beast::error_code ec, std::size_t) {
          auto self = weak.lock();
          if (self == nullptr || ec) {
            CloseStream(*stream);
            return;
          }
          self->active.fetch_add(1);
          const bool keep_alive = parser->get().keep_alive() && !self->stopping;
          const HttpRequest request = ToSmithyRequest(parser->get());
          // Handlers are synchronous for now (ADR-0003 keeps them exception-free);
          // they run on the pool thread that completed the read.
          const HttpResponse response =
              self->handler ? self->handler(request) : HttpResponse{503, {}, ""};
          auto wire = std::make_shared<bhttp::response<bhttp::string_body>>(
              ToWireResponse(response, keep_alive));
          auto& wire_stream = *stream;
          auto& wire_ref = *wire;
          bhttp::async_write(
              wire_stream, wire_ref,
              [weak, stream, wire, keep_alive](beast::error_code write_ec, std::size_t) {
                auto self = weak.lock();
                if (self != nullptr) {
                  self->active.fetch_sub(1);
                }
                if (self == nullptr || write_ec || !keep_alive) {
                  CloseStream(*stream);
                  return;
                }
                self->ReadNext(stream);
              });
        });
  }
};

BeastServerTransport::BeastServerTransport(Options options) : options_(std::move(options)) {}

BeastServerTransport::~BeastServerTransport() { Shutdown(); }

Outcome<Unit> BeastServerTransport::Start(RequestHandler handler) {
  if (state_ != nullptr) {
    return Error::Validation("beast: transport already started");
  }
  auto state = std::make_shared<State>(options_);
  state->handler = std::move(handler);

  const bool has_cert = !options_.tls_certificate_chain_pem.empty();
  const bool has_key = !options_.tls_private_key_pem.empty();
  if (has_cert != has_key) {
    return Error::Validation(
        "beast: TLS termination needs both tls_certificate_chain_pem and tls_private_key_pem");
  }
  if (has_cert) {
    state->ssl.emplace(asio::ssl::context::tls_server);
    boost::system::error_code ssl_ec;
    (void)state->ssl->use_certificate_chain(asio::buffer(options_.tls_certificate_chain_pem),
                                            ssl_ec);
    if (!ssl_ec) {
      (void)state->ssl->use_private_key(asio::buffer(options_.tls_private_key_pem),
                                        asio::ssl::context::pem, ssl_ec);
    }
    if (ssl_ec) {
      return Error::Validation("beast: invalid TLS certificate/key: " + ssl_ec.message());
    }
  }

  boost::system::error_code ec;
  const auto address = asio::ip::make_address(options_.address, ec);
  if (ec) {
    return Error::Validation("beast: invalid bind address: " + options_.address);
  }
  const asio::ip::tcp::endpoint endpoint(address, static_cast<unsigned short>(options_.port));
  (void)state->acceptor.open(endpoint.protocol(), ec);
#ifndef _WIN32
  // Not set on Windows: SO_REUSEADDR there lets a second socket bind a port
  // that is already in use (port hijacking), so conflicting Start()s would
  // silently succeed.
  if (!ec) {
    (void)state->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
  }
#endif
  if (!ec) {
    (void)state->acceptor.bind(endpoint, ec);
  }
  if (!ec) {
    (void)state->acceptor.listen(asio::socket_base::max_listen_connections, ec);
  }
  if (ec) {
    return Error::Transport("beast: cannot listen on " + options_.address + ":" +
                            std::to_string(options_.port) + ": " + ec.message());
  }

  state->Accept();
  state_ = state;
  const int threads = options_.threads > 0 ? options_.threads : 1;
  threads_.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    threads_.emplace_back([state] { state->io.run(); });
  }
  return Unit{};
}

int BeastServerTransport::port() const {
  if (state_ == nullptr) {
    return 0;
  }
  boost::system::error_code ec;
  const auto endpoint = state_->acceptor.local_endpoint(ec);
  return ec ? 0 : endpoint.port();
}

void BeastServerTransport::Stop() { Shutdown(); }

void BeastServerTransport::Shutdown() noexcept {
  if (state_ == nullptr) {
    return;
  }
  state_->stopping = true;
  try {
    // Stop accepting first: close the acceptor on its strand (the accept
    // loop may be touching it on an io thread). The posted closure captures
    // State weakly so an abandoned handler cannot keep State alive.
    asio::post(state_->acceptor.get_executor(), [weak = std::weak_ptr<State>(state_)] {
      auto self = weak.lock();
      if (self != nullptr) {
        boost::system::error_code ignored;
        (void)self->acceptor.close(ignored);
      }
    });
    // Drain: requests already read get up to drain_timeout_seconds to finish
    // writing; keep-alive is off (stopping), so served sessions then close.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(std::max(state_->opts.drain_timeout_seconds, 0));
    while (state_->active.load() > 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Now stop the pool and join; once no io thread is running, nothing may
    // be posted into the io_context (an abandoned handler owning State would
    // form a reference cycle and leak everything).
    state_->io.stop();
    for (std::thread& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  } catch (...) {
    // Teardown must not propagate out of a destructor.
  }
  threads_.clear();
  state_.reset();
}

// ---------------------------------------------------------------------------
// BeastHttpClient
// ---------------------------------------------------------------------------

namespace {

// One dialed connection. Beast's stream timeouts only apply to asynchronous
// operations, so Send() runs async chains and drives the connection's own
// io_context to completion — which also makes concurrent Send() calls
// independent (each uses a distinct connection).
struct ClientConnection {
  ClientConnection() : io(1) {}

  asio::io_context io;
  std::optional<beast::tcp_stream> plain;
  std::optional<asio::ssl::stream<beast::tcp_stream>> tls;

  beast::tcp_stream& lowest() { return tls.has_value() ? beast::get_lowest_layer(*tls) : *plain; }

  // Runs the handlers queued so far to completion, then rearms for reuse.
  void Run() {
    io.run();
    io.restart();
  }
};

}  // namespace

struct BeastHttpClient::State {
  explicit State(Options options) : opts(std::move(options)) {
    if (!opts.tls) {
      return;
    }
    ssl.emplace(asio::ssl::context::tls_client);
    ssl->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                     asio::ssl::context::no_sslv3);
    boost::system::error_code ec;
    if (!opts.verify_peer) {
      ssl->set_verify_mode(asio::ssl::verify_none, ec);
      return;
    }
    ssl->set_verify_mode(asio::ssl::verify_peer, ec);
    if (ec) {
      setup_error = "beast client: cannot enable TLS verification: " + ec.message();
      return;
    }
    if (!opts.ca_pem.empty()) {
      ssl->add_certificate_authority(asio::buffer(opts.ca_pem), ec);
      if (ec) {
        setup_error = "beast client: invalid ca_pem: " + ec.message();
      }
    } else {
      // Best effort: platforms without system OpenSSL paths simply fail
      // verification at handshake time.
      (void)ssl->set_default_verify_paths(ec);
    }
  }

  Options opts;
  std::optional<asio::ssl::context> ssl;
  std::string setup_error;
  std::mutex mutex;
  std::vector<std::unique_ptr<ClientConnection>> idle;

  std::chrono::milliseconds Timeout() const {
    return std::chrono::milliseconds(std::max(opts.request_timeout_ms, 1));
  }

  std::unique_ptr<ClientConnection> TakeIdle() {
    const std::lock_guard<std::mutex> lock(mutex);
    if (idle.empty()) {
      return nullptr;
    }
    auto connection = std::move(idle.back());
    idle.pop_back();
    return connection;
  }

  void ReturnIdle(std::unique_ptr<ClientConnection> connection) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (idle.size() < opts.max_idle_connections) {
      idle.push_back(std::move(connection));
    }
  }

  Outcome<std::unique_ptr<ClientConnection>> Dial() {
    auto connection = std::make_unique<ClientConnection>();
    asio::ip::tcp::resolver resolver(connection->io);
    boost::system::error_code resolve_ec;
    const auto results = resolver.resolve(opts.host, std::to_string(opts.port), resolve_ec);
    if (resolve_ec) {
      return Error::Transport("beast client: cannot resolve " + opts.host + ": " +
                              resolve_ec.message());
    }
    if (opts.tls) {
      connection->tls.emplace(connection->io, *ssl);
      // SNI: virtual-hosted servers need the name before the handshake.
      if (SSL_set_tlsext_host_name(connection->tls->native_handle(), opts.host.c_str()) != 1) {
        return Error::Transport("beast client: cannot set SNI host name");
      }
      if (opts.verify_peer) {
        boost::system::error_code verify_ec;
        connection->tls->set_verify_callback(asio::ssl::host_name_verification(opts.host),
                                             verify_ec);
        if (verify_ec) {
          return Error::Transport("beast client: cannot enable hostname verification: " +
                                  verify_ec.message());
        }
      }
    } else {
      connection->plain.emplace(connection->io);
    }

    connection->lowest().expires_after(Timeout());
    beast::error_code connect_ec;
    connection->lowest().async_connect(
        results,
        [&connect_ec](beast::error_code ec, const asio::ip::tcp::endpoint&) { connect_ec = ec; });
    connection->Run();
    if (connect_ec) {
      return Error::Transport("beast client: cannot connect to " + opts.host + ":" +
                              std::to_string(opts.port) + ": " + connect_ec.message());
    }
    if (opts.tls) {
      connection->lowest().expires_after(Timeout());
      beast::error_code handshake_ec;
      connection->tls->async_handshake(
          asio::ssl::stream_base::client,
          [&handshake_ec](beast::error_code ec) { handshake_ec = ec; });
      connection->Run();
      if (handshake_ec) {
        // Verification failures are configuration/identity problems, not
        // transient transport blips: not retryable.
        return Error::Transport(
            "beast client: TLS handshake with " + opts.host + " failed: " + handshake_ec.message(),
            /*retryable=*/false);
      }
    }
    return connection;
  }

  bhttp::request<bhttp::string_body> ToWireRequest(const HttpRequest& request) const {
    bhttp::request<bhttp::string_body> wire;
    wire.method_string(request.method);
    wire.target(request.target.empty() ? "/" : request.target);
    wire.version(11);
    const int default_port = opts.tls ? 443 : 80;
    wire.set(bhttp::field::host,
             opts.port == default_port ? opts.host : opts.host + ":" + std::to_string(opts.port));
    for (const auto& [name, value] : request.headers.entries()) {
      wire.insert(name, value);
    }
    wire.body() = request.body;
    wire.keep_alive(true);
    wire.prepare_payload();
    return wire;
  }

  // One request/response over the connection. `stale` reports failures that
  // look like a dead keep-alive connection (safe to retry on a fresh dial);
  // `keep_alive` reports whether the response permits reusing the connection.
  Outcome<HttpResponse> RoundTrip(ClientConnection& connection,
                                  const bhttp::request<bhttp::string_body>& wire, bool* stale,
                                  bool* keep_alive) {
    *stale = false;
    *keep_alive = false;
    connection.lowest().expires_after(Timeout());
    beast::error_code write_ec;
    auto write_handler = [&write_ec](beast::error_code ec, std::size_t) { write_ec = ec; };
    if (connection.tls.has_value()) {
      bhttp::async_write(*connection.tls, wire, write_handler);
    } else {
      bhttp::async_write(*connection.plain, wire, write_handler);
    }
    connection.Run();
    if (write_ec) {
      // A reused connection the server already closed fails the write.
      *stale = true;
      return Error::Transport("beast client: write failed: " + write_ec.message());
    }

    beast::flat_buffer buffer;
    bhttp::response_parser<bhttp::string_body> parser;
    parser.body_limit(boost::none);  // Clients accept responses of any size.
    connection.lowest().expires_after(Timeout());
    beast::error_code read_ec;
    auto read_handler = [&read_ec](beast::error_code ec, std::size_t) { read_ec = ec; };
    if (connection.tls.has_value()) {
      bhttp::async_read(*connection.tls, buffer, parser, read_handler);
    } else {
      bhttp::async_read(*connection.plain, buffer, parser, read_handler);
    }
    connection.Run();
    if (read_ec) {
      // EOF before any response bytes: the reused connection went away
      // between requests (retryable on a fresh connection). Anything else
      // (timeout, mid-response reset) is a real failure.
      *stale = read_ec == bhttp::error::end_of_stream && buffer.size() == 0;
      return Error::Transport("beast client: read failed: " + read_ec.message());
    }

    const auto& wire_response = parser.get();
    HttpResponse response;
    response.status = static_cast<int>(wire_response.result_int());
    for (const auto& field : wire_response) {
      response.headers.Add(std::string(field.name_string()), std::string(field.value()));
    }
    response.body = wire_response.body();
    *keep_alive = wire_response.keep_alive();
    return response;
  }
};

BeastHttpClient::BeastHttpClient(Options options)
    : state_(std::make_shared<State>(std::move(options))) {}

BeastHttpClient::~BeastHttpClient() = default;

Outcome<std::shared_ptr<BeastHttpClient>> BeastHttpClient::FromEndpoint(std::string_view url) {
  auto endpoint = ParseEndpoint(url);
  if (!endpoint) {
    return std::move(endpoint).error();
  }
  Options options;
  options.host = endpoint->host;
  options.port = endpoint->port;
  options.tls = endpoint->tls();
  return std::make_shared<BeastHttpClient>(std::move(options));
}

Outcome<HttpResponse> BeastHttpClient::Send(const HttpRequest& request) {
  if (state_->opts.host.empty()) {
    return Error::Validation("beast client: options need a host");
  }
  if (!state_->setup_error.empty()) {
    return Error::Validation(state_->setup_error);
  }
  const auto wire = state_->ToWireRequest(request);

  bool reused = true;
  auto connection = state_->TakeIdle();
  if (connection == nullptr) {
    reused = false;
    auto dialed = state_->Dial();
    if (!dialed) {
      return std::move(dialed).error();
    }
    connection = std::move(dialed).value();
  }

  bool stale = false;
  bool keep_alive = false;
  auto outcome = state_->RoundTrip(*connection, wire, &stale, &keep_alive);
  if (!outcome && reused && stale) {
    // The pooled connection died between requests; retry once on a fresh one.
    auto dialed = state_->Dial();
    if (!dialed) {
      return std::move(dialed).error();
    }
    connection = std::move(dialed).value();
    outcome = state_->RoundTrip(*connection, wire, &stale, &keep_alive);
  }
  if (!outcome) {
    return std::move(outcome).error();
  }
  if (keep_alive) {
    state_->ReturnIdle(std::move(connection));
  }
  return outcome;
}

}  // namespace smithy::http
