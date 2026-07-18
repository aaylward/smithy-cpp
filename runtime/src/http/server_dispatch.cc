#include "smithy/http/server_dispatch.h"

#include <netdb.h>

#include <exception>
#include <iostream>
#include <string>

#include "smithy/core/uuid.h"

namespace smithy::http {
namespace {

HttpResponse InternalError(const HttpRequest& request, const std::string& what) {
  const std::string correlation_id = GenerateUuidV4();
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
  char host[NI_MAXHOST];
  char service[NI_MAXSERV];
  if (getnameinfo(address, length, host, sizeof(host), service, sizeof(service),
                  NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return {};
  }
  return address->sa_family == AF_INET6 ? "[" + std::string(host) + "]:" + service
                                        : std::string(host) + ":" + service;
}

HttpResponse InvokeHandlerGuarded(const RequestHandler& handler, const HttpRequest& request) {
  if (!handler) {
    return HttpResponse{503, {}, "", ""};
  }
  try {
    return handler(request);
  } catch (const std::exception& e) {
    return InternalError(request, e.what());
  } catch (...) {
    return InternalError(request, "unknown exception");
  }
}

}  // namespace smithy::http
