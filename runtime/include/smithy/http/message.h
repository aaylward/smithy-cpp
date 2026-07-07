#ifndef SMITHY_HTTP_MESSAGE_H_
#define SMITHY_HTTP_MESSAGE_H_

#include <string>

#include "smithy/http/headers.h"

namespace smithy::http {

// Request/response bodies are byte strings for now. The alias exists so the
// representation can grow into a stream-shaped type for Phase 8 (event
// streams) without touching every signature.
using Body = std::string;

struct HttpRequest {
  std::string method = "GET";
  // Origin-form target: percent-encoded path plus optional query,
  // e.g. "/cities/a%20b?pageSize=10".
  std::string target = "/";
  Headers headers;
  Body body;
};

struct HttpResponse {
  int status = 200;
  Headers headers;
  Body body;
  // Server-side annotation, never written to the wire: the Smithy operation
  // whose route produced this response (stamped by the generated router so
  // observability middleware can label by operation; empty on 404/405/400
  // dispatch failures and hand-rolled handlers).
  std::string operation;
};

}  // namespace smithy::http

#endif  // SMITHY_HTTP_MESSAGE_H_
