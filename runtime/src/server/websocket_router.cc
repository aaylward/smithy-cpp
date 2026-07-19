#include "smithy/server/websocket_router.h"

#include <algorithm>
#include <utility>

#include "smithy/http/uri.h"

namespace smithy::server {
namespace {

// Router::Route's target normalization: decoded segments with a trailing
// empty segment dropped, so "/a/" matches "/a".
Outcome<http::RequestTarget> NormalizedTarget(const http::HttpRequest& request) {
  auto target = http::ParseRequestTarget(request.target);
  if (!target.ok()) return std::move(target).error();
  std::vector<std::string>& segments = target->path_segments;
  if (!segments.empty() && segments.back().empty()) segments.pop_back();
  return target;
}

}  // namespace

Outcome<Unit> WebSocketRouter::Add(std::string_view method, std::string_view pattern,
                                   StreamServe serve, std::string_view operation) {
  auto segments = internal::ParsePattern(pattern);
  if (!segments) return std::move(segments).error();
  std::vector<StreamRoute>& bucket = routes_.try_emplace(std::string(method)).first->second;
  for (const StreamRoute& existing : bucket) {
    if (internal::SameShape(existing.segments, *segments)) {
      return Error::Validation("router: conflicting route: " + std::string(method) + " " +
                               std::string(pattern));
    }
  }
  bucket.push_back(StreamRoute{std::move(*segments), std::move(serve), std::string(operation)});
  return Unit{};
}

const WebSocketRouter::StreamRoute* WebSocketRouter::FindBest(
    const std::string& method, const std::vector<std::string>& segments) const {
  const auto bucket = routes_.find(method);
  if (bucket == routes_.end()) return nullptr;
  const StreamRoute* best = nullptr;
  for (const StreamRoute& route : bucket->second) {
    if (!internal::MatchSegments(route.segments, segments, nullptr)) continue;
    if (best == nullptr || internal::MoreSpecific(route.segments, best->segments)) best = &route;
  }
  return best;
}

std::function<std::optional<http::HttpResponse>(const http::HttpRequest&)> WebSocketRouter::Gate()
    const {
  return [this](const http::HttpRequest& request) -> std::optional<http::HttpResponse> {
    auto target = NormalizedTarget(request);
    if (!target.ok()) {
      return MakeErrorResponse(400, "BadRequest", "malformed request target");
    }
    if (FindBest(request.method, target->path_segments) != nullptr) {
      return std::nullopt;  // admitted: Serve() will dispatch it
    }
    // Miss path: probe the other methods' buckets for the Allow list, in
    // map (deterministic) order — Router::Route's shapes exactly.
    std::string allow;
    for (const auto& [method, bucket] : routes_) {
      if (method == request.method) continue;
      const bool any_match =
          std::any_of(bucket.begin(), bucket.end(), [&target](const StreamRoute& route) {
            return internal::MatchSegments(route.segments, target->path_segments, nullptr);
          });
      if (!any_match) continue;
      if (!allow.empty()) allow += ", ";
      allow += method;
    }
    if (!allow.empty()) {
      auto response = MakeErrorResponse(405, "MethodNotAllowed", "method not allowed");
      response.headers.Set("allow", allow);
      return response;
    }
    return MakeErrorResponse(404, "NotFound", "no route matches the request");
  };
}

std::function<void(const http::HttpRequest&, http::WebSocket&)> WebSocketRouter::Serve() const {
  return [this](const http::HttpRequest& request, http::WebSocket& socket) {
    auto target = NormalizedTarget(request);
    const StreamRoute* best =
        target.ok() ? FindBest(request.method, target->path_segments) : nullptr;
    if (best == nullptr) {
      socket.Close();  // reachable only when Gate() did not screen the upgrade
      return;
    }
    RequestContext context;
    internal::MatchSegments(best->segments, target->path_segments, &context.labels);
    context.query_params = std::move(target->query_params);
    context.request = &request;
    best->serve(request, context, socket);
  };
}

}  // namespace smithy::server
