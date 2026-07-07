#include "smithy/server/middleware.h"

#include <ranges>
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
      observation.status = response.status;
      observation.duration = std::chrono::duration_cast<std::chrono::milliseconds>(now() - start);
      callback(observation);
      return response;
    };
  };
}

}  // namespace smithy::server
