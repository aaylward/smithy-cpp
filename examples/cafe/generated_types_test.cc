// Behavior tests for the checked-in generated code in examples/cafe/generated
// (regenerate with: cd codegen && gradle generateFixtures).

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "example/cafe/types.h"

namespace example::cafe {
namespace {

// Generated types are ordered (issue #49): operator<=> is defaulted beside
// operator==, so structs, unions, and enums key std::map/std::set and sort.

TEST(GeneratedTypeOrderingTest, StructsKeyOrderedMaps) {
  std::map<GetOrderInput, int> by_input;
  by_input[GetOrderInput{.orderId = "b"}] = 2;
  by_input[GetOrderInput{.orderId = "a"}] = 1;
  EXPECT_EQ(by_input.begin()->first.orderId, "a");
  EXPECT_TRUE(GetOrderInput{.orderId = "a"} < GetOrderInput{.orderId = "b"});
}

TEST(GeneratedTypeOrderingTest, StructsWithEnumAndUnionMembersSort) {
  std::vector<GetOrderOutput> orders;
  orders.push_back(GetOrderOutput{.orderId = "z"});
  orders.push_back(GetOrderOutput{.orderId = "a"});
  std::sort(orders.begin(), orders.end());
  EXPECT_EQ(orders.front().orderId, "a");
}

TEST(GeneratedTypeOrderingTest, UnionsOrderByEngagedMemberThenValue) {
  // monostate < first member < second member; same member orders by value.
  EXPECT_TRUE(OrderStatus{} < OrderStatus::FromPending(PendingStatus{.position = 1}));
  EXPECT_TRUE(OrderStatus::FromPending(PendingStatus{.position = 1}) <
              OrderStatus::FromPending(PendingStatus{.position = 2}));
  EXPECT_TRUE(OrderStatus::FromPending(PendingStatus{.position = 9}) <
              OrderStatus::FromReady(ReadyStatus{}));
}

TEST(GeneratedTypeOrderingTest, UnionsWithUnitMembersStayOrdered) {
  // MilkOption's `none` member is smithy::Unit; if Unit ever lost its <=>,
  // this union's defaulted ordering would silently delete.
  EXPECT_TRUE(MilkOption::FromNone(smithy::Unit{}) <
              MilkOption::FromDairy(DairyMilk{.percentFat = 2.0f}));
}

TEST(GeneratedTypeOrderingTest, EnumsOrderAndCompareAgainstValues) {
  const CoffeeType drip = CoffeeType::Value::kDrip;  // implicit ctor, by design
  EXPECT_TRUE(drip < CoffeeType(CoffeeType::Value::kEspresso));
  // The explicit Value equality friends keep comparisons unambiguous even
  // though the wrapper now converts implicitly to Value.
  EXPECT_TRUE(drip == CoffeeType::Value::kDrip);
  EXPECT_TRUE(CoffeeType::Value::kDrip == drip);
  EXPECT_FALSE(CoffeeType::FromString("OAT_FOAM") == CoffeeType::Value::kDrip);
}

TEST(GeneratedTypeOrderingTest, EnumsSwitchDirectlyWithoutValueCalls) {
  const CoffeeType coffee = CoffeeType::FromString("ESPRESSO");
  std::string picked;
  switch (coffee) {  // issue #49: no `.value()` needed before a switch
    case CoffeeType::Value::kEspresso:
      picked = "espresso";
      break;
    default:
      picked = "other";
      break;
  }
  EXPECT_EQ(picked, "espresso");
}

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
