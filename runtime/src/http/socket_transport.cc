#include "smithy/http/socket_transport.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace smithy::http {
namespace {

#ifdef _WIN32
using SocketFd = SOCKET;
constexpr SocketFd kInvalidSocket = INVALID_SOCKET;

void CloseSocket(SocketFd fd) { closesocket(fd); }

bool EnsureSocketsInitialized() {
  static const bool initialized = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}
#else
using SocketFd = int;
constexpr SocketFd kInvalidSocket = -1;

void CloseSocket(SocketFd fd) { close(fd); }

bool EnsureSocketsInitialized() { return true; }
#endif

void SetTimeouts(SocketFd fd, int timeout_ms) {
#ifdef _WIN32
  const DWORD value = static_cast<DWORD>(timeout_ms);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
  timeval value{};
  value.tv_sec = timeout_ms / 1000;
  value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout_ms % 1000) * 1000L);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
#ifdef SO_NOSIGPIPE
  // macOS/BSD have no MSG_NOSIGNAL; a peer that closes mid-send must surface
  // as an EPIPE write error, never a process-killing SIGPIPE.
  const int no_sigpipe = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
}

bool SendAll(SocketFd fd, std::string_view data) {
#ifdef MSG_NOSIGNAL
  // Linux: writes to a peer-closed socket fail with EPIPE instead of raising
  // SIGPIPE (whose default action would kill the process).
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;  // Windows has no SIGPIPE; macOS uses SO_NOSIGPIPE.
#endif
  while (!data.empty()) {
    const int chunk = static_cast<int>(data.size() > 65536 ? 65536 : data.size());
    const auto sent = send(fd, data.data(), chunk, kSendFlags);
    if (sent <= 0) return false;
    data.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

constexpr std::size_t kMaxHeaderBytes = std::size_t{64} * 1024;
constexpr std::size_t kMaxBodyBytes = std::size_t{64} * 1024 * 1024;

struct ParsedMessage {
  std::string start_line;
  Headers headers;
  std::string body;
};

// Reads one HTTP/1.1 message. For responses without Content-Length the body
// extends to EOF (we always request/emit Connection: close).
Outcome<ParsedMessage> ReadMessage(SocketFd fd, bool body_until_eof) {
  std::string buffer;
  std::size_t header_end = std::string::npos;
  std::array<char, 8192> chunk{};
  while (header_end == std::string::npos) {
    if (buffer.size() > kMaxHeaderBytes) return Error::Transport("http: headers too large");
    const auto received = recv(fd, chunk.data(), static_cast<int>(chunk.size()), 0);
    if (received < 0) return Error::Transport("http: read failed");
    if (received == 0) return Error::Transport("http: connection closed mid-headers");
    buffer.append(chunk.data(), static_cast<std::size_t>(received));
    header_end = buffer.find("\r\n\r\n");
  }

  ParsedMessage message;
  std::string_view head(buffer.data(), header_end);
  const auto line_end = head.find("\r\n");
  message.start_line = std::string(head.substr(0, line_end));
  std::string_view header_block =
      line_end == std::string_view::npos ? std::string_view{} : head.substr(line_end + 2);
  while (!header_block.empty()) {
    const auto eol = header_block.find("\r\n");
    const std::string_view line =
        eol == std::string_view::npos ? header_block : header_block.substr(0, eol);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return Error::Transport("http: malformed header line");
    std::string_view name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.remove_suffix(1);
    }
    message.headers.Add(name, value);
    if (eol == std::string_view::npos) break;
    header_block.remove_prefix(eol + 2);
  }

  // Reject ambiguous or unsupported framing before trusting a body length —
  // the classic request-smuggling desync vectors. Neither transport direction
  // implements chunked transfer, and conflicting content-lengths let a proxy
  // and this server disagree on message boundaries.
  if (message.headers.GetAll("content-length").size() > 1) {
    return Error::Transport("http: conflicting content-length");
  }
  if (message.headers.Has("transfer-encoding")) {
    return Error::Transport("http: transfer-encoding is not supported");
  }

  message.body = buffer.substr(header_end + 4);
  if (const auto length_text = message.headers.Get("content-length")) {
    char* end = nullptr;
    const unsigned long long length = std::strtoull(length_text->c_str(), &end, 10);
    if (end != length_text->c_str() + length_text->size() || length > kMaxBodyBytes) {
      return Error::Transport("http: invalid content-length");
    }
    while (message.body.size() < length) {
      const auto received = recv(fd, chunk.data(), static_cast<int>(chunk.size()), 0);
      if (received <= 0) return Error::Transport("http: connection closed mid-body");
      message.body.append(chunk.data(), static_cast<std::size_t>(received));
    }
    if (message.body.size() != length) return Error::Transport("http: excess body bytes");
  } else if (body_until_eof) {
    while (true) {
      if (message.body.size() > kMaxBodyBytes) return Error::Transport("http: body too large");
      const auto received = recv(fd, chunk.data(), static_cast<int>(chunk.size()), 0);
      if (received < 0) return Error::Transport("http: read failed");
      if (received == 0) break;
      message.body.append(chunk.data(), static_cast<std::size_t>(received));
    }
  }
  return message;
}

}  // namespace

Outcome<HttpResponse> SocketHttpClient::Send(const HttpRequest& request) {
  if (!EnsureSocketsInitialized()) return Error::Transport("http: socket init failed");

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const std::string port_text = std::to_string(port_);
  if (getaddrinfo(host_.c_str(), port_text.c_str(), &hints, &results) != 0 || results == nullptr) {
    return Error::Transport("http: cannot resolve host " + host_);
  }

  SocketFd fd = kInvalidSocket;
  for (const addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (fd == kInvalidSocket) continue;
    SetTimeouts(fd, timeout_ms_);
    if (connect(fd, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) break;
    CloseSocket(fd);
    fd = kInvalidSocket;
  }
  freeaddrinfo(results);
  if (fd == kInvalidSocket) {
    return Error::Transport("http: cannot connect to " + host_ + ":" + port_text);
  }

  std::string wire = request.method + " " + request.target + " HTTP/1.1\r\n";
  wire += "host: " + host_ + ":" + port_text + "\r\n";
  if (!request.headers.Has("content-length")) {
    wire += "content-length: " + std::to_string(request.body.size()) + "\r\n";
  }
  wire += "connection: close\r\n";
  for (const auto& [name, value] : request.headers.entries()) {
    wire += name;
    wire += ": ";
    wire += value;
    wire += "\r\n";
  }
  wire += "\r\n";
  wire += request.body;

  if (!SendAll(fd, wire)) {
    CloseSocket(fd);
    return Error::Transport("http: write failed");
  }
  auto message = ReadMessage(fd, /*body_until_eof=*/true);
  CloseSocket(fd);
  if (!message) return std::move(message).error();

  // Status line: "HTTP/1.1 200 OK".
  const std::string& line = message->start_line;
  const auto space = line.find(' ');
  if (space == std::string::npos || line.size() < space + 4 || line.compare(0, 5, "HTTP/") != 0) {
    return Error::Transport("http: malformed status line: " + line);
  }
  HttpResponse response;
  response.status = std::atoi(line.c_str() + space + 1);
  if (response.status < 100 || response.status > 599) {
    return Error::Transport("http: implausible status in: " + line);
  }
  response.headers = std::move(message->headers);
  response.body = std::move(message->body);
  return response;
}

SocketHttpServer::~SocketHttpServer() { Shutdown(); }

Outcome<Unit> SocketHttpServer::Start(RequestHandler handler) {
  if (!EnsureSocketsInitialized()) return Error::Transport("http: socket init failed");
  handler_ = std::move(handler);
  stopping_ = false;

  const SocketFd fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == kInvalidSocket) return Error::Transport("http: cannot create listener");
#ifndef _WIN32
  const int enable = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<std::uint16_t>(requested_port_));
  if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(fd, 16) != 0) {
    CloseSocket(fd);
    return Error::Transport("http: cannot bind 127.0.0.1:" + std::to_string(requested_port_));
  }
  sockaddr_in bound{};
#ifdef _WIN32
  int bound_size = sizeof(bound);
#else
  socklen_t bound_size = sizeof(bound);
#endif
  getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_size);
  bound_port_ = ntohs(bound.sin_port);
  listener_ = static_cast<std::intptr_t>(fd);
  accept_thread_ = std::thread([this] { AcceptLoop(); });
  return Unit{};
}

void SocketHttpServer::AcceptLoop() {
  const auto listener = static_cast<SocketFd>(listener_);
  while (!stopping_) {
    const SocketFd connection = accept(listener, nullptr, nullptr);
    if (connection == kInvalidSocket) {
      if (stopping_) break;
      continue;
    }
    if (stopping_) {
      CloseSocket(connection);
      break;
    }
    SetTimeouts(connection, 30000);
    auto message = ReadMessage(connection, /*body_until_eof=*/false);
    HttpResponse response;
    if (message) {
      // Request line: "GET /target HTTP/1.1".
      const std::string& line = message->start_line;
      const auto first = line.find(' ');
      const auto second =
          first == std::string::npos ? std::string::npos : line.find(' ', first + 1);
      if (second == std::string::npos) {
        response = HttpResponse{400, {}, "malformed request line"};
      } else {
        HttpRequest request;
        request.method = line.substr(0, first);
        request.target = line.substr(first + 1, second - first - 1);
        request.headers = std::move(message->headers);
        request.body = std::move(message->body);
        response = handler_(request);
      }
    } else {
      response = HttpResponse{400, {}, message.error().message()};
    }

    // The transport is authoritative for framing, so drop any copies a handler
    // set — otherwise they are emitted twice (a duplicate content-length is
    // exactly the smuggling vector a strict peer now rejects).
    response.headers.Remove("content-length");
    response.headers.Remove("connection");
    std::string wire = "HTTP/1.1 " + std::to_string(response.status) + " \r\n";
    wire += "content-length: " + std::to_string(response.body.size()) + "\r\n";
    wire += "connection: close\r\n";
    for (const auto& [name, value] : response.headers.entries()) {
      wire += name;
      wire += ": ";
      wire += value;
      wire += "\r\n";
    }
    wire += "\r\n";
    wire += response.body;
    SendAll(connection, wire);
    CloseSocket(connection);
  }
}

void SocketHttpServer::Stop() { Shutdown(); }

void SocketHttpServer::Shutdown() noexcept {
  if (!accept_thread_.joinable()) return;
  stopping_ = true;
  try {
    // Nudge the accept loop awake with a throwaway connection, then close.
    {
      SocketHttpClient nudge("127.0.0.1", bound_port_, /*timeout_ms=*/1000);
      HttpRequest wake;
      wake.method = "GET";
      wake.target = "/";
      (void)nudge.Send(wake);
    }
    accept_thread_.join();
  } catch (...) {
    // Teardown must not propagate out of a destructor.
  }
  CloseSocket(static_cast<SocketFd>(listener_));
  listener_ = -1;
  handler_ = nullptr;
}

}  // namespace smithy::http
