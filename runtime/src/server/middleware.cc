#include "smithy/server/middleware.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace smithy::server {

http::RequestHandler Chain(std::vector<Middleware> middleware, http::RequestHandler handler) {
  // Wrap inside-out so the first middleware ends up outermost.
  for (const Middleware& wrap : middleware | std::views::reverse) {
    handler = wrap(std::move(handler));
  }
  return handler;
}

Middleware Observe(std::function<void(const RequestObservation&)> callback,
                   std::function<std::chrono::steady_clock::time_point()> now) {
  if (now == nullptr) {
    now = [] { return std::chrono::steady_clock::now(); };
  }
  return [callback = std::move(callback), now = std::move(now)](http::RequestHandler next) {
    return [callback, now, next = std::move(next)](const http::HttpRequest& request) {
      const auto start = now();
      http::HttpResponse response = next(request);
      RequestObservation observation;
      observation.method = request.method;
      observation.target = request.target;
      observation.operation = response.operation;
      observation.trace_parent = request.headers.Get("traceparent").value_or("");
      observation.status = response.status;
      observation.duration = std::chrono::duration_cast<std::chrono::milliseconds>(now() - start);
      callback(observation);
      return response;
    };
  };
}

namespace {

http::HttpResponse Unauthorized() {
  http::HttpResponse response;
  response.status = 401;
  return response;
}

// The credential after "<scheme> " (scheme matched case-insensitively), or
// nullopt when the value does not carry that scheme.
std::optional<std::string> StripScheme(const std::string& value, const std::string& scheme) {
  const std::size_t prefix = scheme.size() + 1;
  if (value.size() <= prefix || value[scheme.size()] != ' ') {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < scheme.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) !=
        std::tolower(static_cast<unsigned char>(scheme[i]))) {
      return std::nullopt;
    }
  }
  return value.substr(prefix);
}

}  // namespace

Middleware RequireBearerAuth(std::function<bool(const std::string&)> validator) {
  return RequireApiKeyHeader("authorization", "Bearer", std::move(validator));
}

Middleware RequireApiKeyHeader(std::string header_name, std::string scheme,
                               std::function<bool(const std::string&)> validator) {
  return [header_name = std::move(header_name), scheme = std::move(scheme),
          validator = std::move(validator)](http::RequestHandler next) {
    return
        [header_name, scheme, validator, next = std::move(next)](const http::HttpRequest& request) {
          std::optional<std::string> credential = request.headers.Get(header_name);
          if (credential.has_value() && !scheme.empty()) {
            credential = StripScheme(*credential, scheme);
          }
          if (!credential.has_value() || !validator(*credential)) {
            return Unauthorized();
          }
          return next(request);
        };
  };
}

}  // namespace smithy::server
