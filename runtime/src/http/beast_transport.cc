#include "smithy/http/beast_transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "smithy/client/config.h"
#include "smithy/http/server_dispatch.h"
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

// The transport is authoritative for framing: keep_alive()/prepare_payload()
// below own these fields, and a handler-set copy would ride along beside
// them — a duplicate or conflicting content-length / transfer-encoding is
// the classic request-smuggling pair (the socket server strips the same
// set; issue #46).
bool IsFramingHeader(std::string_view name) {
  return beast::iequals(name, "content-length") || beast::iequals(name, "transfer-encoding") ||
         beast::iequals(name, "connection");
}

bhttp::response<bhttp::string_body> ToWireResponse(const HttpResponse& response, bool keep_alive) {
  bhttp::response<bhttp::string_body> wire;
  wire.result(static_cast<unsigned>(response.status));
  wire.version(11);
  for (const auto& [name, value] : response.headers.entries()) {
    if (IsFramingHeader(name)) continue;
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

// Lingering-close budgets for over-limit rejections (issue #94): after the
// 413/431 is written, read-and-discard at most this much of the remaining
// request before hard-closing. Unbounded draining would be a DoS vector; a
// client that never reads until it finishes sending may still see a reset —
// inherent to the recipe (nginx/envoy behave the same).
constexpr std::size_t kOverLimitDrainBudgetBytes = std::size_t{256} * 1024;
constexpr std::chrono::seconds kOverLimitDrainDeadline{2};

}  // namespace

// Shared by the acceptor loop, sessions, and the transport object; sessions
// may outlive Stop() briefly, so everything they touch lives here.
struct BeastServerTransport::State : std::enable_shared_from_this<State> {
  explicit State(const Options& options)
      : io(options.threads), acceptor(asio::make_strand(io)), opts(options) {
    if (options.handler_threads > 0) {
      handler_pool =
          std::make_unique<asio::thread_pool>(static_cast<std::size_t>(options.handler_threads));
    }
  }

  asio::io_context io;
  // With handlers on their own pool, the io_context can go momentarily
  // workless — accept paused at the connection cap, a request's read done,
  // its write not yet started because the handler is still running — and
  // io_context::run() would return, killing every io thread mid-request.
  // The guard keeps them in run() until Shutdown's explicit io.stop(),
  // which overrides it.
  asio::executor_work_guard<asio::io_context::executor_type> work_guard{asio::make_work_guard(io)};
  asio::ip::tcp::acceptor acceptor;
  Options opts;
  RequestHandler handler;
  // Handlers run here (when configured) so a blocked handler cannot starve
  // the io threads that accept connections and read/write the wire.
  std::unique_ptr<asio::thread_pool> handler_pool;
  // Engaged when TLS termination is configured (Start validated cert + key).
  std::optional<asio::ssl::context> ssl;
  std::atomic<bool> stopping{false};
  // Requests read off the wire whose responses are not fully written yet;
  // Stop() drains until this reaches zero (or the drain deadline passes).
  std::atomic<int> active{0};

  // Accept-limit state, confined to the acceptor's strand (accept
  // completions and the posted session-closed notifications both run
  // there), so plain non-atomic values are safe.
  std::size_t open_connections = 0;
  bool accept_paused = false;

  // All completion handlers capture State weakly: handlers queued inside the
  // io_context must not own the State that owns the io_context, or abandoned
  // handlers at shutdown would form a reference cycle and leak everything.

  // A connection's stream plus its max_connections accounting. Handlers
  // hold the stream through an aliasing shared_ptr; when the last one lets
  // go the session is destroyed — which is when the fd truly closes, so the
  // count bounds fds, not requests — and the destructor posts the slot
  // release to the acceptor strand, resuming a paused accept loop.
  template <typename Stream>
  struct Session {
    template <typename... Args>
    explicit Session(std::weak_ptr<State> owner, Args&&... args)
        : state(std::move(owner)), stream(std::forward<Args>(args)...) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() {
      auto self = state.lock();
      if (self == nullptr) {
        return;  // Transport already torn down; nothing left to resume.
      }
      try {
        asio::post(self->acceptor.get_executor(), [weak = std::move(state)] {
          auto locked = weak.lock();
          if (locked != nullptr) {
            locked->OnSessionClosed();
          }
        });
      } catch (...) {
        // Allocation failure posting the release leaks one slot; the
        // destructor must not throw.
      }
    }
    std::weak_ptr<State> state;
    Stream stream;
  };

  void Accept() {
    if (opts.max_connections > 0 && open_connections >= opts.max_connections) {
      // At the cap: stop accepting instead of allocating another session.
      // New connections queue in the kernel's listen backlog (bounded; SYNs
      // beyond it drop and retry) — genuine backpressure under a connection
      // flood (issue #46). OnSessionClosed re-arms when a slot frees up.
      accept_paused = true;
      return;
    }
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

  // Posted to the acceptor strand by ~Session when a connection's stream is
  // destroyed (its fd is gone).
  void OnSessionClosed() {
    --open_connections;
    if (accept_paused && !stopping) {
      accept_paused = false;
      Accept();
    }
  }

  void RunSession(asio::ip::tcp::socket socket) {
    ++open_connections;  // Released by ~Session when the fd closes.
    if (!ssl.has_value()) {
      auto session =
          std::make_shared<Session<beast::tcp_stream>>(weak_from_this(), std::move(socket));
      ReadNext(std::shared_ptr<beast::tcp_stream>(session, &session->stream));
      return;
    }
    auto session = std::make_shared<Session<asio::ssl::stream<beast::tcp_stream>>>(
        weak_from_this(), std::move(socket), *ssl);
    std::shared_ptr<asio::ssl::stream<beast::tcp_stream>> stream(session, &session->stream);
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

  // Answers an over-limit request with a minimal 413/431 + Connection: close,
  // then performs a bounded lingering close: half-close the write side and
  // read-and-discard the request's remainder within a small time/byte budget
  // before fully closing. Closing with unread bytes in the kernel's receive
  // buffer would trigger an RST that can destroy the already-sent response in
  // flight — the drain is what makes the status readable (RFC 9112 §9.6;
  // issue #94). One absolute deadline covers the write and the whole drain.
  template <typename Stream>
  void RejectOverLimit(const std::shared_ptr<Stream>& stream, bhttp::status status) {
    auto response = std::make_shared<bhttp::response<bhttp::string_body>>();
    response->result(status);
    response->version(11);
    response->keep_alive(false);
    response->set(bhttp::field::content_type, "text/plain");
    response->body() = status == bhttp::status::payload_too_large
                           ? "request body exceeds the server's limit"
                           : "request headers exceed the server's limit";
    response->prepare_payload();
    beast::get_lowest_layer(*stream).expires_after(kOverLimitDrainDeadline);
    auto& stream_ref = *stream;
    auto& response_ref = *response;
    bhttp::async_write(
        stream_ref, response_ref,
        [weak = weak_from_this(), stream, response](beast::error_code write_ec, std::size_t) {
          CloseStream(*stream);  // half-close: no more writes either way
          auto self = weak.lock();
          if (self == nullptr || write_ec) {
            beast::error_code ignored;
            (void)beast::get_lowest_layer(*stream).socket().close(ignored);
            return;
          }
          self->DrainThenClose(stream, kOverLimitDrainBudgetBytes);
        });
  }

  // Discards raw bytes from the lowest layer (works under TLS too — the
  // discarded ciphertext never needs decrypting) until EOF, error, deadline
  // (set by RejectOverLimit), or budget exhaustion, then closes the socket.
  template <typename Stream>
  void DrainThenClose(const std::shared_ptr<Stream>& stream, std::size_t budget) {
    auto scratch = std::make_shared<std::array<char, 8192>>();
    beast::get_lowest_layer(*stream).async_read_some(
        asio::buffer(*scratch),
        [weak = weak_from_this(), stream, scratch, budget](beast::error_code ec, std::size_t n) {
          auto self = weak.lock();
          if (self == nullptr || ec || n >= budget) {
            beast::error_code ignored;
            (void)beast::get_lowest_layer(*stream).socket().close(ignored);
            return;
          }
          self->DrainThenClose(stream, budget - n);
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
          if (self == nullptr) {
            CloseStream(*stream);
            return;
          }
          if (ec == bhttp::error::body_limit || ec == bhttp::error::header_limit) {
            // Over-limit requests get a real status before the close instead
            // of a bare connection abort (issue #94); every other read error
            // keeps the close-only path below.
            self->RejectOverLimit(stream, ec == bhttp::error::body_limit
                                              ? bhttp::status::payload_too_large
                                              : bhttp::status::request_header_fields_too_large);
            return;
          }
          if (ec) {
            CloseStream(*stream);
            return;
          }
          self->active.fetch_add(1);
          const bool keep_alive = parser->get().keep_alive() && !self->stopping;
          self->Dispatch(stream, ToSmithyRequest(parser->get()), keep_alive);
        });
  }

  // Runs the handler — on the handler pool when configured, else inline on
  // the io thread that completed the read — then hands the response to
  // Respond on the connection's strand. Stream internals (including the
  // per-request timer armed in ReadNext, whose expiry handler runs on that
  // strand) are only ever touched there, so the pool thread never races the
  // stream. InvokeHandlerGuarded contains any exception the handler throws
  // as a 500 — otherwise it would unwind out of the executor and terminate
  // the process, dropping every in-flight request.
  template <typename Stream>
  void Dispatch(const std::shared_ptr<Stream>& stream, HttpRequest request, bool keep_alive) {
    if (handler_pool == nullptr) {
      Respond(stream, InvokeHandlerGuarded(handler, request), keep_alive);
      return;
    }
    asio::post(*handler_pool, [weak = weak_from_this(), stream, request = std::move(request),
                               keep_alive]() mutable {
      auto self = weak.lock();
      if (self == nullptr) {
        return;  // Torn down; the abandoned stream closes the fd.
      }
      HttpResponse response = InvokeHandlerGuarded(self->handler, request);
      asio::post(stream->get_executor(),
                 [weak, stream, response = std::move(response), keep_alive]() {
                   auto self = weak.lock();
                   if (self == nullptr) {
                     CloseStream(*stream);
                     return;
                   }
                   self->Respond(stream, response, keep_alive);
                 });
    });
  }

  template <typename Stream>
  void Respond(const std::shared_ptr<Stream>& stream, const HttpResponse& response,
               bool keep_alive) {
    auto wire =
        std::make_shared<bhttp::response<bhttp::string_body>>(ToWireResponse(response, keep_alive));
    auto& wire_stream = *stream;
    auto& wire_ref = *wire;
    bhttp::async_write(wire_stream, wire_ref,
                       [weak = weak_from_this(), stream, wire, keep_alive](
                           beast::error_code write_ec, std::size_t) {
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
    asio::ssl::context& ssl_context = state->ssl.emplace(asio::ssl::context::tls_server);
    boost::system::error_code ssl_ec;
    (void)ssl_context.use_certificate_chain(asio::buffer(options_.tls_certificate_chain_pem),
                                            ssl_ec);
    if (!ssl_ec) {
      (void)ssl_context.use_private_key(asio::buffer(options_.tls_private_key_pem),
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
  if (!ec) {
    (void)state->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
  }
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
    // Past the deadline: abandon handler work that hasn't started yet
    // (running handlers finish; their responses are dropped once io stops).
    if (state_->handler_pool != nullptr) {
      state_->handler_pool->stop();
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
    // Wait for any still-running handlers; like the io joins above, this is
    // unbounded if a handler is truly wedged (issue #46's remaining drain
    // item — parity, not a regression).
    if (state_->handler_pool != nullptr) {
      state_->handler_pool->join();
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
  // Exactly one of these is set, chosen when the connection is dialed.
  std::unique_ptr<beast::tcp_stream> plain;
  std::unique_ptr<asio::ssl::stream<beast::tcp_stream>> tls;

  beast::tcp_stream& lowest() const {
    return tls != nullptr ? beast::get_lowest_layer(*tls) : *plain;
  }

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
    asio::ssl::context& ssl_context = ssl.emplace(asio::ssl::context::tls_client);
    ssl_context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                            asio::ssl::context::no_sslv3);
    boost::system::error_code ec;
    if (!opts.tls_options.verify_peer) {
      (void)ssl_context.set_verify_mode(asio::ssl::verify_none, ec);
      return;
    }
    (void)ssl_context.set_verify_mode(asio::ssl::verify_peer, ec);
    if (ec) {
      setup_error = "beast client: cannot enable TLS verification: " + ec.message();
      return;
    }
    if (!opts.tls_options.ca_pem.empty()) {
      (void)ssl_context.add_certificate_authority(asio::buffer(opts.tls_options.ca_pem), ec);
      if (ec) {
        setup_error = "beast client: invalid ca_pem: " + ec.message();
      }
    } else {
      // Best effort: platforms without system OpenSSL paths simply fail
      // verification at handshake time.
      (void)ssl_context.set_default_verify_paths(ec);
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
      if (!ssl.has_value()) {
        return Error::Validation("beast client: TLS context was not configured");
      }
      asio::ssl::context& ssl_context = *ssl;
      connection->tls =
          std::make_unique<asio::ssl::stream<beast::tcp_stream>>(connection->io, ssl_context);
      // SNI: virtual-hosted servers need the name before the handshake.
      if (SSL_set_tlsext_host_name(connection->tls->native_handle(), opts.host.c_str()) != 1) {
        return Error::Transport("beast client: cannot set SNI host name");
      }
      if (opts.tls_options.verify_peer) {
        boost::system::error_code verify_ec;
        (void)connection->tls->set_verify_callback(asio::ssl::host_name_verification(opts.host),
                                                   verify_ec);
        if (verify_ec) {
          return Error::Transport("beast client: cannot enable hostname verification: " +
                                  verify_ec.message());
        }
      }
    } else {
      connection->plain = std::make_unique<beast::tcp_stream>(connection->io);
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
                                  bool* keep_alive) const {
    *stale = false;
    *keep_alive = false;
    connection.lowest().expires_after(Timeout());
    beast::error_code write_ec;
    auto write_handler = [&write_ec](beast::error_code ec, std::size_t) { write_ec = ec; };
    if (connection.tls != nullptr) {
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
    if (connection.tls != nullptr) {
      bhttp::async_read(*connection.tls, buffer, parser, read_handler);
    } else {
      bhttp::async_read(*connection.plain, buffer, parser, read_handler);
    }
    connection.Run();
    if (read_ec) {
      // A failure before any response bytes means the reused connection went
      // away between requests (clean EOF on Linux, ECONNRESET on macOS) —
      // retryable on a fresh connection. Timeouts and mid-response failures
      // are real; a redial would only repeat them.
      *stale = !parser.got_some() && read_ec != beast::error::timeout;
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

Outcome<std::shared_ptr<BeastHttpClient>> BeastHttpClient::FromConfig(const ClientConfig& config) {
  auto endpoint = ParseEndpoint(config.endpoint);
  if (!endpoint) {
    return std::move(endpoint).error();
  }
  Options options;
  options.host = endpoint->host;
  options.port = endpoint->port;
  options.tls = endpoint->tls();
  options.tls_options = config.tls;
  options.request_timeout_ms = config.request_timeout_ms;
  options.max_idle_connections = config.max_idle_connections;
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
