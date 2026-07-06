#include "smithy/http/beast_transport.h"

#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <memory>
#include <utility>

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
  std::atomic<bool> stopping{false};

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
    auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
    ReadNext(stream);
  }

  void ReadNext(const std::shared_ptr<beast::tcp_stream>& stream) {
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto parser = std::make_shared<bhttp::request_parser<bhttp::string_body>>();
    parser->body_limit(opts.max_body_bytes);
    stream->expires_after(std::chrono::seconds(opts.request_timeout_seconds));
    auto& stream_ref = *stream;
    bhttp::async_read(
        stream_ref, *buffer, *parser,
        [weak = weak_from_this(), stream, buffer, parser](beast::error_code ec, std::size_t) {
          auto self = weak.lock();
          if (self == nullptr || ec) {
            beast::error_code ignored;
            (void)stream->socket().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
            return;
          }
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
                if (self == nullptr || write_ec || !keep_alive) {
                  beast::error_code ignored;
                  (void)stream->socket().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
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
    // Stop the pool and join; once no io thread is running, it is safe to
    // touch the acceptor directly (nothing may be posted into the io_context
    // here — an abandoned handler would keep State alive and leak it).
    state_->io.stop();
    for (std::thread& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    boost::system::error_code ignored;
    (void)state_->acceptor.close(ignored);
  } catch (...) {
    // Teardown must not propagate out of a destructor.
  }
  threads_.clear();
  state_.reset();
}

}  // namespace smithy::http
