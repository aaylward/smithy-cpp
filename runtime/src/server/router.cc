#include "smithy/server/router.h"

#include <algorithm>
#include <utility>

#include "smithy/http/uri.h"

namespace smithy::server {

http::HttpResponse MakeErrorResponse(int status, std::string_view code, std::string_view message) {
  http::HttpResponse response;
  response.status = status;
  response.headers.Set("content-type", "application/json");
  // Both fields are JSON-string-safe for the inputs the runtime produces.
  response.body =
      "{\"code\":\"" + std::string(code) + "\",\"message\":\"" + std::string(message) + "\"}";
  return response;
}

Outcome<std::vector<Router::Segment>> Router::ParsePattern(std::string_view pattern) {
  if (pattern.empty() || pattern[0] != '/') {
    return Error::Validation("router: pattern must start with '/': " + std::string(pattern));
  }
  std::vector<Segment> segments;
  std::string_view rest = pattern.substr(1);
  if (rest.empty()) return segments;  // "/" => zero segments
  while (true) {
    const auto slash = rest.find('/');
    const std::string_view raw = slash == std::string_view::npos ? rest : rest.substr(0, slash);
    Segment segment;
    if (raw.size() >= 2 && raw.front() == '{' && raw.back() == '}') {
      std::string_view name = raw.substr(1, raw.size() - 2);
      if (name.ends_with('+')) {
        segment.kind = Segment::Kind::kGreedy;
        name.remove_suffix(1);
      } else {
        segment.kind = Segment::Kind::kLabel;
      }
      if (name.empty()) {
        return Error::Validation("router: empty label in pattern: " + std::string(pattern));
      }
      segment.text = std::string(name);
    } else if (raw.empty()) {
      return Error::Validation("router: empty segment in pattern: " + std::string(pattern));
    } else {
      segment.kind = Segment::Kind::kLiteral;
      segment.text = std::string(raw);
    }
    const bool greedy = segment.kind == Segment::Kind::kGreedy;
    segments.push_back(std::move(segment));
    if (slash == std::string_view::npos) break;
    if (greedy) {
      return Error::Validation("router: greedy label must be the final segment: " +
                               std::string(pattern));
    }
    rest.remove_prefix(slash + 1);
  }
  return segments;
}

Outcome<Unit> Router::Add(std::string_view method, std::string_view pattern, RouteHandler handler,
                          std::string_view operation) {
  auto segments = ParsePattern(pattern);
  if (!segments) return std::move(segments).error();
  std::vector<RouteEntry>& bucket = routes_.try_emplace(std::string(method)).first->second;
  for (const RouteEntry& existing : bucket) {
    if (existing.segments.size() != segments->size()) continue;
    bool same_shape = true;
    for (std::size_t i = 0; i < segments->size(); ++i) {
      const Segment& a = existing.segments[i];
      const Segment& b = (*segments)[i];
      if (a.kind != b.kind || (a.kind == Segment::Kind::kLiteral && a.text != b.text)) {
        same_shape = false;
        break;
      }
    }
    if (same_shape) {
      return Error::Validation("router: conflicting route: " + std::string(method) + " " +
                               std::string(pattern));
    }
  }
  bucket.push_back(RouteEntry{std::move(*segments), std::move(handler), std::string(operation)});
  return Unit{};
}

bool Router::Matches(const RouteEntry& route, const std::vector<std::string>& segments,
                     PathLabels* labels) {
  if (labels != nullptr) labels->clear();
  const bool has_greedy =
      !route.segments.empty() && route.segments.back().kind == Segment::Kind::kGreedy;
  if (has_greedy) {
    if (segments.size() < route.segments.size()) return false;
  } else if (segments.size() != route.segments.size()) {
    return false;
  }
  for (std::size_t i = 0; i < route.segments.size(); ++i) {
    const Segment& expected = route.segments[i];
    switch (expected.kind) {
      case Segment::Kind::kLiteral:
        if (segments[i] != expected.text) return false;
        break;
      case Segment::Kind::kLabel:
        if (segments[i].empty()) return false;
        if (labels != nullptr) labels->emplace(expected.text, segments[i]);
        break;
      case Segment::Kind::kGreedy: {
        // The joined value ("seg/seg/...") is empty only when a lone empty
        // segment remains: two or more always meet at a '/'.
        if (segments.size() - i == 1 && segments[i].empty()) return false;
        if (labels != nullptr) {
          std::string joined;
          for (std::size_t j = i; j < segments.size(); ++j) {
            if (j > i) joined.push_back('/');
            joined += segments[j];
          }
          labels->emplace(expected.text, std::move(joined));
        }
        return true;
      }
    }
  }
  return true;
}

bool Router::MoreSpecific(const std::vector<Segment>& a, const std::vector<Segment>& b) {
  // Segment-by-segment: literal beats label beats greedy. Longer patterns
  // rank higher when a prefix ties.
  const std::size_t common = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < common; ++i) {
    const auto rank = [](const Segment& s) {
      switch (s.kind) {
        case Segment::Kind::kLiteral:
          return 0;
        case Segment::Kind::kLabel:
          return 1;
        case Segment::Kind::kGreedy:
          return 2;
      }
      return 3;
    };
    if (rank(a[i]) != rank(b[i])) return rank(a[i]) < rank(b[i]);
  }
  return a.size() > b.size();
}

http::HttpResponse Router::Route(const http::HttpRequest& request) const {
  const auto target = http::ParseRequestTarget(request.target);
  if (!target) {
    return MakeErrorResponse(400, "BadRequest", "malformed request target");
  }
  // Drop a trailing empty segment so "/a/" matches "/a".
  std::vector<std::string> segments = target->path_segments;
  if (!segments.empty() && segments.back().empty()) segments.pop_back();

  const RouteEntry* best = nullptr;
  if (const auto bucket = routes_.find(request.method); bucket != routes_.end()) {
    for (const RouteEntry& route : bucket->second) {
      if (!Matches(route, segments, nullptr)) continue;
      if (best == nullptr || MoreSpecific(route.segments, best->segments)) best = &route;
    }
  }
  if (best == nullptr) {
    // Miss path only: probe the other methods' buckets for the Allow list —
    // each contributes at most once, in map (deterministic) order.
    std::string allow;
    for (const auto& [method, bucket] : routes_) {
      if (method == request.method) continue;
      const bool any_match = std::any_of(
          bucket.begin(), bucket.end(),
          [&segments](const RouteEntry& route) { return Matches(route, segments, nullptr); });
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
  }
  RequestContext context;
  Matches(*best, segments, &context.labels);  // label extraction: winner only
  context.query_params = target->query_params;
  http::HttpResponse response = best->handler(request, context);
  if (!best->operation.empty()) response.operation = best->operation;
  return response;
}

}  // namespace smithy::server
