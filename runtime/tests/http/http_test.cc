#include <gtest/gtest.h>

#include "smithy/http/headers.h"
#include "smithy/http/loopback.h"
#include "smithy/http/message.h"
#include "smithy/http/uri.h"

namespace smithy::http {
namespace {

TEST(HeadersTest, GetIsCaseInsensitive) {
  Headers headers;
  headers.Add("Content-Type", "application/json");
  EXPECT_EQ(headers.Get("content-type"), "application/json");
  EXPECT_EQ(headers.Get("CONTENT-TYPE"), "application/json");
  EXPECT_TRUE(headers.Has("Content-type"));
  EXPECT_FALSE(headers.Has("content-length"));
  EXPECT_EQ(headers.Get("missing"), std::nullopt);
}

TEST(HeadersTest, PreservesRepeatedValuesInOrder) {
  Headers headers;
  headers.Add("X-Tag", "a");
  headers.Add("x-tag", "b");
  EXPECT_EQ(headers.GetAll("X-TAG"), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(headers.Get("x-tag"), "a");
  headers.Set("X-Tag", "only");
  EXPECT_EQ(headers.GetAll("x-tag"), (std::vector<std::string>{"only"}));
  headers.Remove("X-TAG");
  EXPECT_FALSE(headers.Has("x-tag"));
}

TEST(UriTest, EncodesPathSegmentsPerSmithySpec) {
  // Unreserved characters pass through; everything else is escaped.
  EXPECT_EQ(EncodePathSegment("azAZ09-_.~"), "azAZ09-_.~");
  EXPECT_EQ(EncodePathSegment("a b"), "a%20b");
  EXPECT_EQ(EncodePathSegment("a/b"), "a%2Fb");
  EXPECT_EQ(EncodePathSegment("a:b?c#d[e]f"), "a%3Ab%3Fc%23d%5Be%5Df");
  EXPECT_EQ(EncodePathSegment("café"), "caf%C3%A9");
  // Greedy labels keep '/' but escape everything else.
  EXPECT_EQ(EncodeGreedyPathSegment("a/b c/d"), "a/b%20c/d");
}

TEST(UriTest, PercentDecodeRoundTripsAndRejectsMalformed) {
  const auto decoded = PercentDecode("caf%C3%A9%20%2F%20bar");
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(*decoded, "café / bar");
  EXPECT_FALSE(PercentDecode("%2").ok());
  EXPECT_FALSE(PercentDecode("%GG").ok());
  EXPECT_FALSE(PercentDecode("100%").ok());
}

TEST(UriTest, BuildsQueryStrings) {
  QueryString query;
  EXPECT_EQ(query.ToString(), "");
  query.Add("pageSize", "10");
  query.Add("filter", "a b&c");
  query.Add("empty", "");
  query.AddFlag("flag");
  EXPECT_EQ(query.ToString(), "?pageSize=10&filter=a%20b%26c&empty=&flag");
}

TEST(HeadersTest, SplitHeaderListValues) {
  EXPECT_EQ(SplitHeaderListValues("a, b,c"), (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_EQ(SplitHeaderListValues("one"), (std::vector<std::string>{"one"}));
  EXPECT_EQ(SplitHeaderListValues("a, ,b"), (std::vector<std::string>{"a", "", "b"}));
  EXPECT_EQ(SplitHeaderListValues(""), (std::vector<std::string>{""}));
}

TEST(HeadersTest, MediaTypeOfStripsParametersAndCase) {
  EXPECT_EQ(MediaTypeOf("application/json"), "application/json");
  EXPECT_EQ(MediaTypeOf("Application/JSON; charset=utf-8"), "application/json");
  EXPECT_EQ(MediaTypeOf("  text/plain ; q=1"), "text/plain");
  EXPECT_EQ(MediaTypeOf(""), "");
}

TEST(HeadersTest, SplitHttpDateHeaderValues) {
  EXPECT_EQ(
      SplitHttpDateHeaderValues("Mon, 16 Dec 2019 23:48:18 GMT, Mon, 16 Dec 2019 23:48:18 GMT"),
      (std::vector<std::string>{"Mon, 16 Dec 2019 23:48:18 GMT", "Mon, 16 Dec 2019 23:48:18 GMT"}));
  EXPECT_EQ(SplitHttpDateHeaderValues("Mon, 16 Dec 2019 23:48:18 GMT"),
            (std::vector<std::string>{"Mon, 16 Dec 2019 23:48:18 GMT"}));
  EXPECT_TRUE(SplitHttpDateHeaderValues("").empty());
}

TEST(UriTest, QueryStringHasMatchesRawKeys) {
  QueryString query;
  EXPECT_FALSE(query.Has("a b"));
  query.Add("a b", "1");
  EXPECT_TRUE(query.Has("a b")) << "keys compare pre-encoding";
  EXPECT_FALSE(query.Has("a%20b"));
  EXPECT_FALSE(query.Has("other"));
}

TEST(UriTest, ParsesRequestTargets) {
  const auto target = ParseRequestTarget("/cities/a%20b/forecast?pageSize=10&flag&q=x%26y");
  ASSERT_TRUE(target.ok());
  EXPECT_EQ(target->path_segments, (std::vector<std::string>{"cities", "a b", "forecast"}));
  ASSERT_EQ(target->query_params.size(), 3u);
  EXPECT_EQ(target->query_params[0], (std::pair<std::string, std::string>{"pageSize", "10"}));
  EXPECT_EQ(target->query_params[1], (std::pair<std::string, std::string>{"flag", ""}));
  EXPECT_EQ(target->query_params[2], (std::pair<std::string, std::string>{"q", "x&y"}));
}

TEST(UriTest, ParsesRootAndTrailingSlashes) {
  const auto root = ParseRequestTarget("/");
  ASSERT_TRUE(root.ok());
  EXPECT_TRUE(root->path_segments.empty());
  const auto trailing = ParseRequestTarget("/a/");
  ASSERT_TRUE(trailing.ok());
  EXPECT_EQ(trailing->path_segments, (std::vector<std::string>{"a", ""}));
  EXPECT_FALSE(ParseRequestTarget("no-slash").ok());
  EXPECT_FALSE(ParseRequestTarget("/bad%2").ok());
}

TEST(UriTest, ParsesEndpoints) {
  const auto plain = ParseEndpoint("http://localhost");
  ASSERT_TRUE(plain.ok());
  EXPECT_EQ(plain->host, "localhost");
  EXPECT_EQ(plain->port, 80);
  EXPECT_EQ(plain->path_prefix, "");

  const auto full = ParseEndpoint("http://127.0.0.1:8080/api/v1/");
  ASSERT_TRUE(full.ok());
  EXPECT_EQ(full->host, "127.0.0.1");
  EXPECT_EQ(full->port, 8080);
  EXPECT_EQ(full->path_prefix, "/api/v1");

  EXPECT_FALSE(ParseEndpoint("https://secure.example.com").ok());
  EXPECT_FALSE(ParseEndpoint("http://").ok());
  EXPECT_FALSE(ParseEndpoint("http://host:0").ok());
  EXPECT_FALSE(ParseEndpoint("http://host:notaport").ok());
  EXPECT_FALSE(ParseEndpoint("host:80").ok());
}

TEST(LoopbackTest, RoutesRequestsToHandler) {
  Loopback loopback;
  HttpRequest probe;
  probe.target = "/echo";
  EXPECT_FALSE(loopback.Send(probe).ok());  // no handler yet

  ASSERT_TRUE(loopback
                  .Start([](const HttpRequest& request) {
                    HttpResponse response;
                    response.status = 201;
                    response.headers.Set("x-echo-target", request.target);
                    response.body = request.body;
                    return response;
                  })
                  .ok());

  probe.method = "POST";
  probe.body = "ping";
  const auto response = loopback.Send(probe);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->status, 201);
  EXPECT_EQ(response->headers.Get("X-Echo-Target"), "/echo");
  EXPECT_EQ(response->body, "ping");

  loopback.Stop();
  EXPECT_FALSE(loopback.Send(probe).ok());
}

TEST(LoopbackTest, AsyncSendUsesSameHandler) {
  Loopback loopback;
  ASSERT_TRUE(loopback.Start([](const HttpRequest&) { return HttpResponse{204, {}, ""}; }).ok());
  HttpRequest request;
  auto future = loopback.SendAsync(request);
  const auto response = future.get();
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->status, 204);
}

}  // namespace
}  // namespace smithy::http
