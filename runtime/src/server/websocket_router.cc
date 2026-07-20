#include "smithy/server/websocket_router.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "smithy/http/uri.h"

namespace smithy::server {

Outcome<Unit> WebSocketRouter::Add(std::string_view method, std::string_view pattern,
                                   StreamServe serve, std::string_view operation) {
  if (seam_ == Seam::kShared) {
    return Error::Validation(
        "router: this router already serves the shared seam (AddSession/on_websocket_session); "
        "one router mounts one dispatcher, so its routes serve one seam");
  }
  auto segments = internal::ParsePattern(pattern);
  if (!segments) return std::move(segments).error();
  std::vector<StreamRoute>& bucket = routes_.try_emplace(std::string(method)).first->second;
  for (const StreamRoute& existing : bucket) {
    if (internal::SameShape(existing.segments, *segments)) {
      return Error::Validation("router: conflicting route: " + std::string(method) + " " +
                               std::string(pattern));
    }
  }
  StreamRoute route;
  route.segments = std::move(*segments);
  route.serve = std::move(serve);
  route.operation = std::string(operation);
  bucket.push_back(std::move(route));
  seam_ = Seam::kBorrowed;
  return Unit{};
}

Outcome<Unit> WebSocketRouter::AddSession(std::string_view method, std::string_view pattern,
                                          StreamServeSession serve, std::string_view operation) {
  if (seam_ == Seam::kBorrowed) {
    return Error::Validation(
        "router: this router already serves the borrowed seam (Add/on_websocket); one router "
        "mounts one dispatcher, so its routes serve one seam");
  }
  auto segments = internal::ParsePattern(pattern);
  if (!segments) return std::move(segments).error();
  std::vector<StreamRoute>& bucket = routes_.try_emplace(std::string(method)).first->second;
  for (const StreamRoute& existing : bucket) {
    if (internal::SameShape(existing.segments, *segments)) {
      return Error::Validation("router: conflicting route: " + std::string(method) + " " +
                               std::string(pattern));
    }
  }
  StreamRoute route;
  route.segments = std::move(*segments);
  route.serve_session = std::move(serve);
  route.operation = std::string(operation);
  bucket.push_back(std::move(route));
  seam_ = Seam::kShared;
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
    auto target = internal::NormalizedTarget(request);
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
    auto target = internal::NormalizedTarget(request);
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

std::function<void(const http::HttpRequest&, std::shared_ptr<http::WebSocket>)>
WebSocketRouter::ServeSession() const {
  return [this](const http::HttpRequest& request, std::shared_ptr<http::WebSocket> socket) {
    auto target = internal::NormalizedTarget(request);
    const StreamRoute* best =
        target.ok() ? FindBest(request.method, target->path_segments) : nullptr;
    if (best == nullptr) {
      socket->Close();  // reachable only when Gate() did not screen the upgrade
      return;
    }
    RequestContext context;
    internal::MatchSegments(best->segments, target->path_segments, &context.labels);
    context.query_params = std::move(target->query_params);
    context.request = &request;
    best->serve_session(request, context, std::move(socket));
  };
}

}  // namespace smithy::server
