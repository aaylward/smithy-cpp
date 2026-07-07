// Phase 4 inverse bridge test: the hand-written weather client from Phase 1
// talks to the GENERATED restJson1 weather server — the counterpart of
// generated_client_e2e_test.cc, closing the loop before the Phase 5 harness.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "example/weather/client.h"
#include "example/weather/server.h"
#include "examples/weather/handwritten/weather_client.h"
#include "smithy/client/interceptor.h"
#include "smithy/client/observability.h"
#include "smithy/http/loopback.h"
#include "smithy/http/message.h"
#include "smithy/http/trace_context.h"
#include "smithy/server/middleware.h"

namespace example::weather {
namespace {

namespace hw = example::weather::handwritten;

// Reference implementation of the GENERATED handler interface.
class ReferenceHandler : public WeatherHandler {
 public:
  smithy::Outcome<GetCityOutput> GetCity(const GetCityInput& input) override {
    if (input.cityId != "seattle") {
      smithy::Error error = smithy::Error::Modeled("NoSuchResource", "no city: " + input.cityId);
      error.set_detail(NoSuchResource{.resourceType = "City"});
      return error;
    }
    return GetCityOutput{
        .name = "Seattle",
        .coordinates = CityCoordinates{.latitude = 47.6062F, .longitude = -122.3321F}};
  }

  smithy::Outcome<DeleteCityOutput> DeleteCity(const DeleteCityInput& input) override {
    if (input.cityId != "seattle") {
      smithy::Error error = smithy::Error::Modeled("NoSuchResource", "no city: " + input.cityId);
      error.set_detail(NoSuchResource{.resourceType = "City"});
      return error;
    }
    return DeleteCityOutput{};
  }

  smithy::Outcome<ListCitiesOutput> ListCities(const ListCitiesInput& input) override {
    ListCitiesOutput out;
    if (!input.nextToken.has_value()) {
      out.items.push_back(CitySummary{.cityId = "seattle", .name = "Seattle"});
      if (input.pageSize.value_or(2) < 2) {
        out.nextToken = "page-2";
        return out;
      }
    }
    out.items.push_back(CitySummary{.cityId = "rain city", .name = "Rain City"});
    return out;
  }

  smithy::Outcome<GetForecastOutput> GetForecast(const GetForecastInput& input) override {
    (void)input;
    return GetForecastOutput{.chanceOfRain = 0.75F};
  }

  smithy::Outcome<GetCurrentTimeOutput> GetCurrentTime(const GetCurrentTimeInput& input) override {
    (void)input;
    return GetCurrentTimeOutput{.time = smithy::Timestamp::FromEpochMilliseconds(1398796238500)};
  }

  smithy::Outcome<GetReportOutput> GetReport(const GetReportInput& input) override {
    // Echo the decoded path so tests can assert label-decoding fidelity.
    return GetReportOutput{.path = input.reportPath,
                           .sizeBytes = static_cast<std::int64_t>(input.reportPath.size())};
  }
};

class GeneratedServerEndToEndTest : public testing::Test {
 protected:
  void SetUp() override {
    server_ = std::make_unique<WeatherServer>(std::make_shared<ReferenceHandler>());
    auto loopback = std::make_shared<smithy::http::Loopback>();
    ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
    smithy::ClientConfig config;
    config.http_client = loopback;
    auto client = hw::WeatherClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<hw::WeatherClient>(std::move(*client));
  }

  std::unique_ptr<WeatherServer> server_;
  std::unique_ptr<hw::WeatherClient> client_;
};

TEST_F(GeneratedServerEndToEndTest, GetCityRoundTrips) {
  const auto city = client_->GetCity(hw::GetCityInput{.cityId = "seattle"});
  ASSERT_TRUE(city.ok()) << city.error().message();
  EXPECT_EQ(city->name, "Seattle");
  EXPECT_FLOAT_EQ(city->coordinates.latitude, 47.6062F);
  EXPECT_FLOAT_EQ(city->coordinates.longitude, -122.3321F);
}

// A transport that fails transiently proves the generated client's retry
// path end to end (Phase 7): the third attempt reaches the server.
class FlakyTransport final : public smithy::http::HttpClient {
 public:
  explicit FlakyTransport(std::shared_ptr<smithy::http::HttpClient> inner)
      : inner_(std::move(inner)) {}

  smithy::Outcome<smithy::http::HttpResponse> Send(
      const smithy::http::HttpRequest& request) override {
    if (++calls_ <= 2) return smithy::Error::Transport("transient outage");
    return inner_->Send(request);
  }

 private:
  std::shared_ptr<smithy::http::HttpClient> inner_;
  int calls_ = 0;
};

TEST_F(GeneratedServerEndToEndTest, GeneratedClientRetriesTransientFailures) {
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
  smithy::ClientConfig config;
  config.http_client = std::make_shared<FlakyTransport>(loopback);
  config.retry.sleep = [](std::chrono::milliseconds) {};  // instant for tests
  auto client = example::weather::WeatherClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());
  const auto city = client->GetCity(example::weather::GetCityInput{.cityId = "seattle"});
  ASSERT_TRUE(city.ok()) << city.error().message();
  EXPECT_EQ(city->name, "Seattle");
}

// Client interceptor + server middleware together (Phase 7b): the interceptor
// injects a bearer token on every generated-client request; server middleware
// rejects requests without it before the router runs, and Observe reports the
// served request.
TEST_F(GeneratedServerEndToEndTest, InterceptorAndMiddlewareCarryAuthAcrossTheWire) {
  class BearerAuth final : public smithy::Interceptor {
   public:
    void ModifyBeforeTransmit(smithy::http::HttpRequest& request, int) override {
      request.headers.Set("authorization", "Bearer smoke-token");
    }
  };

  std::vector<smithy::server::RequestObservation> observations;
  auto require_auth = [](smithy::http::RequestHandler next) {
    return [next = std::move(next)](const smithy::http::HttpRequest& request) {
      if (request.headers.Get("authorization") != "Bearer smoke-token") {
        smithy::http::HttpResponse response;
        response.status = 401;
        return response;
      }
      return next(request);
    };
  };
  auto handler = smithy::server::Chain(
      {require_auth, smithy::server::Observe([&](const smithy::server::RequestObservation& o) {
         observations.push_back(o);
       })},
      server_->Handler());

  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(handler).ok());

  // Without the interceptor the middleware rejects the call outright.
  {
    smithy::ClientConfig config;
    config.http_client = loopback;
    config.retry.max_attempts = 1;
    auto client = example::weather::WeatherClient::Create(std::move(config));
    ASSERT_TRUE(client.ok());
    const auto city = client->GetCity(example::weather::GetCityInput{.cityId = "seattle"});
    ASSERT_FALSE(city.ok());
  }

  smithy::ClientConfig config;
  config.http_client = loopback;
  config.interceptors.push_back(std::make_shared<BearerAuth>());
  auto client = example::weather::WeatherClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());
  const auto city = client->GetCity(example::weather::GetCityInput{.cityId = "seattle"});
  ASSERT_TRUE(city.ok()) << city.error().message();
  EXPECT_EQ(city->name, "Seattle");

  // Observe sits inside the auth check, so only the authorized call reports.
  ASSERT_EQ(observations.size(), 1u);
  EXPECT_EQ(observations[0].method, "GET");
  EXPECT_EQ(observations[0].target, "/cities/seattle");
  EXPECT_EQ(observations[0].status, 200);
}

// @httpBearerAuth end to end (Phase 7c): config.bearer_token feeds the
// generated client; RequireBearerAuth guards the generated server.
TEST_F(GeneratedServerEndToEndTest, BearerTokenFlowsFromConfigThroughAuthMiddleware) {
  auto handler =
      smithy::server::Chain({smithy::server::RequireBearerAuth(
                                [](const std::string& token) { return token == "weather-token"; })},
                            server_->Handler());
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(handler).ok());

  {
    smithy::ClientConfig config;
    config.http_client = loopback;
    config.retry.max_attempts = 1;
    auto anonymous = example::weather::WeatherClient::Create(std::move(config));
    ASSERT_TRUE(anonymous.ok());
    EXPECT_FALSE(anonymous->GetCity(example::weather::GetCityInput{.cityId = "seattle"}).ok());
  }

  smithy::ClientConfig config;
  config.http_client = loopback;
  config.bearer_token = [] { return std::string("weather-token"); };
  auto client = example::weather::WeatherClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());
  const auto city = client->GetCity(example::weather::GetCityInput{.cityId = "seattle"});
  ASSERT_TRUE(city.ok()) << city.error().message();
  EXPECT_EQ(city->name, "Seattle");
}

// The generated @paginated paginator walks pages until the server stops
// returning a next token (ReferenceHandler pages when pageSize < 2).
TEST_F(GeneratedServerEndToEndTest, PaginatorWalksAllPages) {
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
  smithy::ClientConfig config;
  config.http_client = loopback;
  auto client = example::weather::WeatherClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  auto paginator = client->PaginateListCities(example::weather::ListCitiesInput{.pageSize = 1});

  const auto first = paginator.Next();
  ASSERT_TRUE(first.ok()) << first.error().message();
  ASSERT_TRUE(first->has_value());
  ASSERT_EQ((*first)->items.size(), 1u);
  EXPECT_EQ((*first)->items[0].cityId, "seattle");

  const auto second = paginator.Next();
  ASSERT_TRUE(second.ok()) << second.error().message();
  ASSERT_TRUE(second->has_value());
  ASSERT_EQ((*second)->items.size(), 1u);
  EXPECT_EQ((*second)->items[0].cityId, "rain city");

  const auto done = paginator.Next();
  ASSERT_TRUE(done.ok());
  EXPECT_FALSE(done->has_value());
  // Exhausted paginators stay exhausted.
  const auto still_done = paginator.Next();
  ASSERT_TRUE(still_done.ok());
  EXPECT_FALSE(still_done->has_value());
}

// Observability end to end (Phase 7c): the client propagates a W3C trace
// context and observes its attempts; the server's Observe middleware reports
// the matched operation and the incoming traceparent for correlation.
TEST_F(GeneratedServerEndToEndTest, TraceContextAndOperationFlowThroughObservability) {
  std::vector<smithy::server::RequestObservation> served;
  auto handler = smithy::server::Chain(
      {smithy::server::Observe(
          [&](const smithy::server::RequestObservation& o) { served.push_back(o); })},
      server_->Handler());
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(handler).ok());

  std::vector<smithy::AttemptObservation> attempts;
  smithy::ClientConfig config;
  config.http_client = loopback;
  config.interceptors.push_back(smithy::PropagateTraceContext());
  config.interceptors.push_back(
      smithy::ObserveAttempts([&](const smithy::AttemptObservation& a) { attempts.push_back(a); }));
  auto client = example::weather::WeatherClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  const auto city = client->GetCity(example::weather::GetCityInput{.cityId = "seattle"});
  ASSERT_TRUE(city.ok()) << city.error().message();

  ASSERT_EQ(served.size(), 1u);
  EXPECT_EQ(served[0].operation, "GetCity");
  const auto trace = smithy::http::ParseTraceparent(served[0].trace_parent);
  ASSERT_TRUE(trace.has_value()) << served[0].trace_parent;
  EXPECT_TRUE(trace->sampled);

  ASSERT_EQ(attempts.size(), 1u);
  EXPECT_EQ(attempts[0].attempt, 1);
  EXPECT_EQ(attempts[0].status, 200);
  EXPECT_EQ(attempts[0].method, "GET");

  // Dispatch failures report an empty operation.
  smithy::http::HttpRequest unrouted;
  unrouted.method = "GET";
  unrouted.target = "/no/such/route";
  (void)handler(unrouted);
  ASSERT_EQ(served.size(), 2u);
  EXPECT_EQ(served[1].status, 404);
  EXPECT_TRUE(served[1].operation.empty());
}

// Greedy labels ({reportPath+}) keep their embedded slashes and decode
// percent-encoded characters segment by segment.
TEST_F(GeneratedServerEndToEndTest, GreedyLabelKeepsSlashesAndDecodes) {
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
  smithy::ClientConfig config;
  config.http_client = loopback;
  auto client = example::weather::WeatherClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  for (const std::string path : {
           std::string("2026/q3/summary.pdf"),       // plain multi-segment
           std::string("a b/c%d/e?f"),               // space, percent, question mark
           std::string("weird&seg=ment/#frag/two"),  // ampersand, equals, hash
           std::string("caf\xc3\xa9/na\xc3\xafve"),  // UTF-8 segments
       }) {
    const auto report = client->GetReport(example::weather::GetReportInput{.reportPath = path});
    ASSERT_TRUE(report.ok()) << "path: " << path << ": " << report.error().message();
    EXPECT_EQ(report->path, path);
    EXPECT_EQ(report->sizeBytes, static_cast<std::int64_t>(path.size()));
  }
}

// Hostile strings in a plain @httpLabel. CityId is @pattern-constrained to
// "^[A-Za-z0-9 ]+$", so this pins two behaviors: values inside the charset
// (including tricky spacing) round-trip exactly — the handler's error message
// echoes the decoded id, proving encode -> route -> decode fidelity — and
// URI-hostile values outside it are rejected by generated validation with a
// clean 400 ValidationException, never a crash, mis-route, or mangled echo.
TEST_F(GeneratedServerEndToEndTest, HostileLabelValuesSurviveTheRoundTrip) {
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
  smithy::ClientConfig config;
  config.http_client = loopback;
  auto client = example::weather::WeatherClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  for (const std::string city : {
           std::string("abc def"),    // %20 in the label segment
           std::string("a  b"),       // consecutive spaces survive
           std::string(" leading"),   // leading space
           std::string("trailing "),  // trailing space
           std::string("UPPER lower 0123456789"),
       }) {
    const auto outcome = client->GetCity(example::weather::GetCityInput{.cityId = city});
    ASSERT_FALSE(outcome.ok()) << "city: " << city;
    EXPECT_EQ(outcome.error().code(), "NoSuchResource") << city;
    EXPECT_EQ(outcome.error().message(), "no city: " + city);
  }

  for (const std::string city : {
           std::string("san jos\xc3\xa9"),  // UTF-8 outside the pattern
           std::string("x?y#z&w=v"),        // every URI delimiter
           std::string("100%25 legit"),     // literal percent-escape text
           std::string("+plus+signs+"),     // '+' must not decode to space
           std::string("dot./..dot"),       // dot segments must not normalize
       }) {
    const auto outcome = client->GetCity(example::weather::GetCityInput{.cityId = city});
    ASSERT_FALSE(outcome.ok()) << "city: " << city;
    EXPECT_EQ(outcome.error().code(), "ValidationException")
        << city << ": " << outcome.error().message();
  }
}

// A pagination token full of URI-hostile characters must survive the
// output-body -> input-query round trip the paginator drives.
TEST_F(GeneratedServerEndToEndTest, PaginatorRoundTripsHostileTokens) {
  static const std::string kNastyToken = "a b&c=d?e+f/g%h#i";
  class PagingHandler final : public ReferenceHandler {
   public:
    smithy::Outcome<ListCitiesOutput> ListCities(const ListCitiesInput& input) override {
      ListCitiesOutput out;
      if (!input.nextToken.has_value()) {
        out.items.push_back(CitySummary{.cityId = "page1", .name = "Page One"});
        out.nextToken = kNastyToken;
        return out;
      }
      // The token must arrive exactly as issued, through the query string.
      if (*input.nextToken != kNastyToken) {
        return smithy::Error::Modeled("NoSuchResource", "token corrupted: " + *input.nextToken);
      }
      out.items.push_back(CitySummary{.cityId = "page2", .name = "Page Two"});
      return out;
    }
  };

  WeatherServer server(std::make_shared<PagingHandler>());
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server.Handler()).ok());
  smithy::ClientConfig config;
  config.http_client = loopback;
  auto client = example::weather::WeatherClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  auto paginator = client->PaginateListCities(example::weather::ListCitiesInput{});
  const auto first = paginator.Next();
  ASSERT_TRUE(first.ok()) << first.error().message();
  ASSERT_TRUE(first->has_value());
  EXPECT_EQ((*first)->items[0].cityId, "page1");

  const auto second = paginator.Next();
  ASSERT_TRUE(second.ok()) << second.error().message();
  ASSERT_TRUE(second->has_value()) << "pagination ended early";
  EXPECT_EQ((*second)->items[0].cityId, "page2");

  const auto done = paginator.Next();
  ASSERT_TRUE(done.ok());
  EXPECT_FALSE(done->has_value());
}

// Everything at once: transient transport failures under retry, bearer auth
// from config, trace propagation and attempt observation via interceptors,
// auth + observation middleware on the server. Client-side hooks see every
// attempt; the server sees exactly one authorized request.
TEST_F(GeneratedServerEndToEndTest, FullStackLayeringSurvivesRetries) {
  std::vector<smithy::server::RequestObservation> served;
  auto handler = smithy::server::Chain(
      {smithy::server::RequireBearerAuth(
           [](const std::string& token) { return token == "stack-token"; }),
       smithy::server::Observe(
           [&](const smithy::server::RequestObservation& o) { served.push_back(o); })},
      server_->Handler());
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(handler).ok());

  std::vector<smithy::AttemptObservation> attempts;
  smithy::ClientConfig config;
  config.http_client = std::make_shared<FlakyTransport>(loopback);  // 2 failures first
  config.retry.sleep = [](std::chrono::milliseconds) {};
  config.bearer_token = [] { return std::string("stack-token"); };
  config.interceptors.push_back(smithy::PropagateTraceContext());
  config.interceptors.push_back(
      smithy::ObserveAttempts([&](const smithy::AttemptObservation& a) { attempts.push_back(a); }));
  auto client = example::weather::WeatherClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  const auto city = client->GetCity(example::weather::GetCityInput{.cityId = "seattle"});
  ASSERT_TRUE(city.ok()) << city.error().message();

  ASSERT_EQ(attempts.size(), 3u);
  EXPECT_EQ(attempts[0].status, -1);
  EXPECT_EQ(attempts[1].status, -1);
  EXPECT_EQ(attempts[2].status, 200);
  EXPECT_EQ(attempts[2].attempt, 3);

  // Only the successful attempt reached the server, authorized and traced.
  ASSERT_EQ(served.size(), 1u);
  EXPECT_EQ(served[0].operation, "GetCity");
  EXPECT_TRUE(smithy::http::ParseTraceparent(served[0].trace_parent).has_value());
}

TEST_F(GeneratedServerEndToEndTest, DeleteCityIs204WithNoBody) {
  smithy::http::HttpRequest request;
  request.method = "DELETE";
  request.target = "/cities/seattle";
  const smithy::http::HttpResponse response = server_->Handler()(request);
  EXPECT_EQ(response.status, 204);
  EXPECT_TRUE(response.body.empty()) << response.body;
  EXPECT_FALSE(response.headers.Get("content-type").has_value());
}

TEST_F(GeneratedServerEndToEndTest, ModeledErrorsGetTheirHttpStatusAndCode) {
  const auto city = client_->GetCity(hw::GetCityInput{.cityId = "atlantis"});
  ASSERT_FALSE(city.ok());
  EXPECT_EQ(city.error().kind(), smithy::ErrorKind::kModeled);
  EXPECT_EQ(city.error().code(), "NoSuchResource");  // @httpError(404) on the wire
  EXPECT_EQ(city.error().message(), "no city: atlantis");
}

TEST_F(GeneratedServerEndToEndTest, QueryBindingsPaginate) {
  const auto first =
      client_->ListCities(hw::ListCitiesInput{.nextToken = std::nullopt, .pageSize = 1});
  ASSERT_TRUE(first.ok()) << first.error().message();
  ASSERT_EQ(first->items.size(), 1u);
  ASSERT_TRUE(first->nextToken.has_value());

  const auto second =
      client_->ListCities(hw::ListCitiesInput{.nextToken = first->nextToken, .pageSize = 1});
  ASSERT_TRUE(second.ok());
  ASSERT_EQ(second->items.size(), 1u);
  EXPECT_EQ(second->items[0].cityId, "rain city");
}

TEST_F(GeneratedServerEndToEndTest, TimestampsRoundTrip) {
  const auto time = client_->GetCurrentTime();
  ASSERT_TRUE(time.ok()) << time.error().message();
  EXPECT_EQ(time->time.epoch_milliseconds(), 1398796238500);
}

}  // namespace
}  // namespace example::weather
