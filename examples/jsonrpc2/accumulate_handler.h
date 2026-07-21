#ifndef SMITHY_EXAMPLES_JSONRPC2_ACCUMULATE_HANDLER_H_
#define SMITHY_EXAMPLES_JSONRPC2_ACCUMULATE_HANDLER_H_

// The reference Calculator handlers every stream suite drives — the
// in-memory pair e2e and the Beast e2e, blocking and async — one place so
// the seams cannot drift. Accumulate seeds the running total from the
// opening call's `start`, answers each non-zero term with the new total, a
// zero term ends the session cleanly (the "=" key), and passing
// kAccumulateLimit ends it with the modeled Overflow — the terminal error
// envelope on the wire (ADR-0023).

#include <string>

#include "example/calculator/server.h"
#include "smithy/core/outcome.h"

namespace example::calculator {

inline constexpr double kAccumulateLimit = 100.0;

inline smithy::Error MakeOverflowError() {
  smithy::Error overflow = smithy::Error::Modeled("Overflow", "accumulator over the limit");
  overflow.set_detail(
      Overflow{.message = "accumulator over the limit", .limit = kAccumulateLimit});
  return overflow;
}

class AccumulatingCalculator final : public CalculatorHandler {
 public:
  smithy::Outcome<AddOutput> Add(const AddInput& input,
                                 const smithy::server::RequestContext&) override {
    return AddOutput{.sum = input.a + input.b};
  }

  smithy::Outcome<DivideOutput> Divide(const DivideInput& input,
                                       const smithy::server::RequestContext&) override {
    if (input.divisor == 0) {
      smithy::Error error = smithy::Error::Modeled("DivisionByZero", "division by zero");
      error.set_detail(DivisionByZero{.message = "division by zero"});
      return error;
    }
    return DivideOutput{.quotient = input.dividend / input.divisor};
  }

  smithy::Outcome<smithy::Unit> Accumulate(const AccumulateInput& input,
                                           AccumulateServerStream& stream,
                                           const smithy::server::RequestContext&) override {
    double total = input.start.value_or(0.0);
    while (true) {
      auto term = stream.Receive();
      if (!term.ok() || !term->has_value()) return smithy::Unit{};  // wire failed or client left
      const double value = (**term).as_add().value;
      if (value == 0) return smithy::Unit{};  // the "=" key: clean end, terminal result
      total += value;
      if (total > kAccumulateLimit) return MakeOverflowError();
      if (!stream.Send(Totals::FromTotal(RunningTotal{.value = total})).ok()) {
        return smithy::Unit{};
      }
    }
  }
};

class AsyncAccumulatingCalculator final : public CalculatorAsyncHandler {
 public:
  smithy::Outcome<AddOutput> Add(const AddInput& input,
                                 const smithy::server::RequestContext&) override {
    return AddOutput{.sum = input.a + input.b};
  }

  smithy::Outcome<DivideOutput> Divide(const DivideInput& input,
                                       const smithy::server::RequestContext&) override {
    if (input.divisor == 0) {
      smithy::Error error = smithy::Error::Modeled("DivisionByZero", "division by zero");
      error.set_detail(DivisionByZero{.message = "division by zero"});
      return error;
    }
    return DivideOutput{.quotient = input.dividend / input.divisor};
  }

  smithy::eventstream::StreamTask Accumulate(AccumulateInput input,
                                             AccumulateAsyncServerStream& stream) override {
    double total = input.start.value_or(0.0);
    while (true) {
      auto term = co_await stream.Receive();
      if (!term.ok() || !term->has_value()) co_return smithy::Unit{};
      const double value = (**term).as_add().value;
      if (value == 0) co_return smithy::Unit{};
      total += value;
      if (total > kAccumulateLimit) co_return MakeOverflowError();
      auto sent = co_await stream.Send(Totals::FromTotal(RunningTotal{.value = total}));
      if (!sent.ok()) co_return smithy::Unit{};
    }
  }
};

}  // namespace example::calculator

#endif  // SMITHY_EXAMPLES_JSONRPC2_ACCUMULATE_HANDLER_H_
