#ifndef SMITHY_HTTP_URI_H_
#define SMITHY_HTTP_URI_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "smithy/core/outcome.h"

namespace smithy::http {

// Percent-encodes for a single path segment (httpLabel): every byte outside
// RFC 3986 "unreserved" is escaped, including '/'.
std::string EncodePathSegment(std::string_view text);

// Percent-encodes for a greedy httpLabel: like EncodePathSegment but '/' is
// kept, since greedy labels span multiple segments.
std::string EncodeGreedyPathSegment(std::string_view text);

// Percent-encodes a query key or value (httpQuery).
std::string EncodeQueryComponent(std::string_view text);

// Decodes %XX escapes. Rejects malformed escapes.
Outcome<std::string> PercentDecode(std::string_view text);

// Accumulates query parameters into "?k=v&k2=v2" form with encoding applied.
class QueryString {
 public:
  void Add(std::string_view key, std::string_view value);
  // "" when empty, otherwise "?...".
  std::string ToString() const;

 private:
  std::vector<std::pair<std::string, std::string>> params_;
};

// Splits a raw target like "/cities/a%20b?page=2&size=10" into a decoded
// segment list and raw key/value pairs (values percent-decoded). Rejects
// malformed escapes.
struct RequestTarget {
  std::vector<std::string> path_segments;                         // decoded
  std::vector<std::pair<std::string, std::string>> query_params;  // decoded
};
Outcome<RequestTarget> ParseRequestTarget(std::string_view target);

// Minimal endpoint parser for "http://host[:port][/prefix]".
struct Endpoint {
  std::string scheme;  // "http" (https arrives with TLS-capable transports)
  std::string host;
  int port = 80;
  std::string path_prefix;  // no trailing '/', possibly empty
};
Outcome<Endpoint> ParseEndpoint(std::string_view url);

}  // namespace smithy::http

#endif  // SMITHY_HTTP_URI_H_
