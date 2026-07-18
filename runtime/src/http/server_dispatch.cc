#include "smithy/http/server_dispatch.h"

#include <netdb.h>

#include <array>
#include <exception>
#include <iostream>
#include <string>

#include "smithy/http/trace_context.h"

namespace smithy::http {
namespace {

// The mint half of the guard's contract (server_dispatch.h, ADR-0011).
// The size() == 1 gate: W3C counts duplicated traceparent headers as
// malformed, so a duplicate restarts the trace like any other bad input.
void EnsureInboundTraceIdentity(HttpRequest& request) {
  const auto values = request.headers.GetAll("traceparent");
  if (values.size() == 1 && ParseTraceparent(values.front()).has_value()) {
    return;
  }
  request.headers.Set("traceparent", FormatTraceparent(GenerateTraceContext()));
  request.headers.Remove("tracestate");
}

HttpResponse InternalError(const HttpRequest& request, const std::string& what) {
  // The correlation id is the request's trace id (issues #41/#46).
  // EnsureInboundTraceIdentity ran before the handler, so this parse cannot
  // fail; re-parsing here keeps the common path free of an extra id copy
  // while this rare path pays it.
  const std::string correlation_id =
      ParseTraceparent(request.headers.Get("traceparent").value_or(""))
          .value_or(TraceContext{})
          .trace_id;
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

// A 5xx that leaves the handler chain without a correlation id gets the
// request's trace id (issue #46) — a returned smithy::Error mapped to a 500
// by a generated server then correlates exactly like a thrown one, across
// every protocol, with no generated code involved. A handler-set id wins.
void CorrelateServerError(const HttpRequest& request, HttpResponse& response) {
  if (response.status < 500 || response.headers.Has("x-correlation-id")) {
    return;
  }
  const auto trace = ParseTraceparent(request.headers.Get("traceparent").value_or(""));
  if (trace.has_value()) {
    response.headers.Set("x-correlation-id", trace->trace_id);
  }
}

HttpResponse InvokeHandlerGuarded(const RequestHandler& handler, HttpRequest request) {
  if (!handler) {
    return HttpResponse{503, {}, "", ""};
  }
  EnsureInboundTraceIdentity(request);
  try {
    HttpResponse response = handler(request);
    CorrelateServerError(request, response);
    return response;
  } catch (const std::exception& e) {
    return InternalError(request, e.what());
  } catch (...) {
    return InternalError(request, "unknown exception");
  }
}

}  // namespace smithy::http
