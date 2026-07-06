#ifndef SMITHY_SERVER_ROUTER_H_
#define SMITHY_SERVER_ROUTER_H_

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "smithy/core/outcome.h"
#include "smithy/http/message.h"

namespace smithy::server {

// Values captured from URI labels during route matching, keyed by label name.
// Greedy label values keep their embedded slashes (decoded).
using PathLabels = std::map<std::string, std::string, std::less<>>;

// Per-request context passed to matched handlers.
struct RequestContext {
  PathLabels labels;
  std::vector<std::pair<std::string, std::string>> query_params;  // decoded
};

using RouteHandler =
    std::function<http::HttpResponse(const http::HttpRequest&, const RequestContext&)>;

// Method + URI-pattern dispatch per the Smithy HTTP binding spec.
//
// Patterns use @http trait syntax: "/cities/{cityId}/forecast" or greedy
// "/files/{path+}". Matching precedence follows the spec: at each segment a
// literal outranks a label, and a label outranks a greedy label; the most
// specific matching route wins regardless of registration order.
class Router {
 public:
  // Fails on invalid patterns and on route conflicts (same method + pattern
  // shape) — the generator surfaces this at build time.
  Outcome<Unit> Add(std::string_view method, std::string_view pattern, RouteHandler handler);

  // Full dispatch: 404 (no pattern match), 405 (pattern match, wrong method,
  // with an Allow header), 400 (malformed target); otherwise the handler's
  // response.
  http::HttpResponse Route(const http::HttpRequest& request) const;

 private:
  struct Segment {
    enum class Kind { kLiteral, kLabel, kGreedy } kind = Kind::kLiteral;
    std::string text;  // literal text or label name
  };
  struct RouteEntry {
    std::string method;
    std::vector<Segment> segments;
    RouteHandler handler;
  };

  static Outcome<std::vector<Segment>> ParsePattern(std::string_view pattern);
  static bool Matches(const RouteEntry& route, const std::vector<std::string>& segments,
                      PathLabels* labels);
  // True when `a` is more specific than `b` per the spec's precedence rules.
  static bool MoreSpecific(const std::vector<Segment>& a, const std::vector<Segment>& b);

  std::vector<RouteEntry> routes_;
};

// Uniform error response used by the router and by generated servers for
// framework-level failures (protocol-specific error bodies land in Phase 4).
http::HttpResponse MakeErrorResponse(int status, std::string_view code, std::string_view message);

// Raised (as a value) by generated input validation before user handlers run.
struct ValidationFailure {
  std::string path;  // member path, e.g. "input.cityId"
  std::string message;
};

}  // namespace smithy::server

#endif  // SMITHY_SERVER_ROUTER_H_
