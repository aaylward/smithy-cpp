#include "smithy/http/server_dispatch.h"

#include <netdb.h>

#include <array>
#include <exception>
#include <iostream>
#include <string>

#include "smithy/core/uuid.h"
#include "smithy/http/trace_context.h"

namespace smithy::http {
namespace {

// The request's trace identity (ADR-0011): a valid inbound traceparent is
// kept verbatim (the caller's trace continues), while an absent or malformed
// one is replaced with a fresh root context — the W3C restart-the-trace
// rule. After this, every request in the handler chain carries a parseable
// traceparent: Observe's trace_parent is never empty, and a handler can
// always derive child spans from context.request.
void EnsureInboundTraceIdentity(HttpRequest& request) {
  const auto header = request.headers.Get("traceparent");
  if (header.has_value() && ParseTraceparent(*header).has_value()) {
    return;
  }
  request.headers.Set("traceparent", FormatTraceparent(GenerateTraceContext()));
}

HttpResponse InternalError(const HttpRequest& request, const std::string& what) {
  // The correlation id is the request's trace id (minted at ingress when the
  // client sent none), so the clog line, the 500 body, and any distributed
  // trace all name one identity (issues #41/#46). The uuid fallback only
  // fires for callers that bypassed the guard's minting.
  const auto trace = ParseTraceparent(request.headers.Get("traceparent").value_or(""));
  const std::string correlation_id = trace.has_value() ? trace->trace_id : GenerateUuidV4();
  // The built-in default sink. A structured-logging seam can replace this, but
  // an unhandled handler exception must never be silent — this line is often
  // the only server-side trace of a 500.
  std::clog << "smithy: handler threw; correlation-id=" << correlation_id << " request=\""
            << request.method << ' ' << request.target << "\" what=\"" << what << "\"\n";
  HttpResponse response;
  response.status = 500;
  response.headers.Set("content-type", "application/json");
  response.headers.Set("x-correlation-id", correlation_id);
  response.body = "{\"message\":\"internal error\",\"correlationId\":\"" + correlation_id + "\"}";
  return response;
}

}  // namespace

std::string FormatPeerAddress(const sockaddr* address, socklen_t length) {
  std::array<char, NI_MAXHOST> host{};
  std::array<char, NI_MAXSERV> service{};
  if (getnameinfo(address, length, host.data(), host.size(), service.data(), service.size(),
                  NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return {};
  }
  return address->sa_family == AF_INET6 ? "[" + std::string(host.data()) + "]:" + service.data()
                                        : std::string(host.data()) + ":" + service.data();
}

HttpResponse InvokeHandlerGuarded(const RequestHandler& handler, HttpRequest request) {
  if (!handler) {
    return HttpResponse{503, {}, "", ""};
  }
  EnsureInboundTraceIdentity(request);
  try {
    return handler(request);
  } catch (const std::exception& e) {
    return InternalError(request, e.what());
  } catch (...) {
    return InternalError(request, "unknown exception");
  }
}

}  // namespace smithy::http
