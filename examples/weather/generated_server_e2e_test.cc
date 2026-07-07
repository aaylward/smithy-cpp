// Phase 4 inverse bridge test: the hand-written weather client from Phase 1
// talks to the GENERATED restJson1 weather server — the counterpart of
// generated_client_e2e_test.cc, closing the loop before the Phase 5 harness.

#include <gtest/gtest.h>

#include <memory>

#include "example/weather/server.h"
#include "examples/weather/handwritten/weather_client.h"
#include "smithy/http/loopback.h"

namespace example::weather {
namespace {

namespace hw = example::weather::handwritten;

// Reference implementation of the GENERATED handler interface.
class ReferenceHandler final : public WeatherHandler {
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
