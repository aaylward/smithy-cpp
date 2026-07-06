// Behavior tests for the checked-in generated code in examples/cafe/generated
// (regenerate with: cd codegen && gradle generateFixtures).

#include <gtest/gtest.h>

#include "example/cafe/types.h"

namespace example::cafe {
namespace {

TEST(CafeGeneratedTypesTest, EnumRoundTripsKnownValues) {
  const CoffeeType drip = CoffeeType::FromString("DRIP");
  EXPECT_EQ(drip.value(), CoffeeType::Value::kDrip);
  EXPECT_EQ(drip.ToString(), "DRIP");
  EXPECT_EQ(drip, CoffeeType(CoffeeType::Value::kDrip));
  EXPECT_EQ(CoffeeType::FromString("LATTE").value(), CoffeeType::Value::kLatte);
}

TEST(CafeGeneratedTypesTest, EnumPreservesUnknownValues) {
  const CoffeeType mystery = CoffeeType::FromString("OAT_FOAM");
  EXPECT_EQ(mystery.value(), CoffeeType::Value::kUnknown);
  EXPECT_EQ(mystery.ToString(), "OAT_FOAM");  // original wire text survives
  EXPECT_FALSE(mystery == CoffeeType::FromString("SOY_FOAM"));
  EXPECT_EQ(mystery, CoffeeType::FromString("OAT_FOAM"));
}

TEST(CafeGeneratedTypesTest, EnumDefaultIsUnknown) {
  const CoffeeType none;
  EXPECT_EQ(none.value(), CoffeeType::Value::kUnknown);
  EXPECT_EQ(none.ToString(), "");
}

TEST(CafeGeneratedTypesTest, UnionFactoriesAndAccessors) {
  const MilkOption dairy = MilkOption::FromDairy(DairyMilk{.percentFat = 2.0F});
  EXPECT_FALSE(dairy.empty());
  EXPECT_TRUE(dairy.is_dairy());
  EXPECT_FALSE(dairy.is_none());
  EXPECT_FALSE(dairy.is_alternative());
  EXPECT_FLOAT_EQ(dairy.as_dairy().percentFat, 2.0F);

  const MilkOption none = MilkOption::FromNone(smithy::Unit{});
  EXPECT_TRUE(none.is_none());

  const MilkOption alt = MilkOption::FromAlternative(AlternativeMilk{.kind = "oat"});
  EXPECT_EQ(alt.as_alternative().kind, "oat");
}

TEST(CafeGeneratedTypesTest, UnionDefaultIsEmptyAndEqualityWorks) {
  const MilkOption unset;
  EXPECT_TRUE(unset.empty());
  EXPECT_EQ(unset, MilkOption{});
  EXPECT_FALSE(unset == MilkOption::FromNone(smithy::Unit{}));
  EXPECT_EQ(MilkOption::FromDairy(DairyMilk{.percentFat = 2.0F}),
            MilkOption::FromDairy(DairyMilk{.percentFat = 2.0F}));
}

TEST(CafeGeneratedTypesTest, NestedUnionInsideStructs) {
  GetOrderOutput output{.orderId = "o-1",
                        .coffeeType = CoffeeType(CoffeeType::Value::kCortado),
                        .status = OrderStatus::FromPending(PendingStatus{.position = 3})};
  ASSERT_TRUE(output.status.is_pending());
  EXPECT_EQ(output.status.as_pending().position, 3);
  output.status = OrderStatus::FromReady(
      ReadyStatus{.readyAt = smithy::Timestamp::FromEpochMilliseconds(5000)});
  EXPECT_TRUE(output.status.is_ready());
}

TEST(CafeGeneratedTypesTest, InputStructMixesRequiredAndOptional) {
  const OrderCoffeeInput input{.coffeeType = CoffeeType(CoffeeType::Value::kEspresso)};
  EXPECT_FALSE(input.milk.has_value());
  EXPECT_FALSE(input.clientToken.has_value());
  EXPECT_EQ(input.coffeeType.value(), CoffeeType::Value::kEspresso);
}

}  // namespace
}  // namespace example::cafe
