#ifndef SMITHY_HTTP_SERVER_DISPATCH_H_
#define SMITHY_HTTP_SERVER_DISPATCH_H_

#include <sys/socket.h>

#include <string>

#include "smithy/http/message.h"
#include "smithy/http/transport.h"

namespace smithy::http {

// Renders a peer's sockaddr as HttpRequest::peer_address ("ip:port", v6
// bracketed) — the one definition of the format, shared by every server
// transport so the stamps cannot drift. Empty when the address cannot be
// rendered.
std::string FormatPeerAddress(const sockaddr* address, socklen_t length);

// Invokes handler(request), converting any exception that escapes the handler
// into a 500 response instead of letting it propagate. Server transports call
// this rather than invoking the handler directly, so a throwing handler fails
// exactly one request instead of unwinding out of the transport's I/O thread
// and terminating the whole process.
//
// The synthesized 500 carries a generated correlation id in the
// "x-correlation-id" header (and a minimal JSON body repeating it); the same id
// plus the exception's what() is written to std::clog, so an otherwise-silent
// crash leaves one greppable line tying the client-visible failure to a
// server-side cause. handler may be empty — that yields a 503 (no correlation
// id: nothing ran).
HttpResponse InvokeHandlerGuarded(const RequestHandler& handler, const HttpRequest& request);

}  // namespace smithy::http

#endif  // SMITHY_HTTP_SERVER_DISPATCH_H_
