#ifndef SMITHY_CORE_OVERLOADED_H_
#define SMITHY_CORE_OVERLOADED_H_

namespace smithy {

// Overload-set builder for visiting generated unions (or any std::variant):
//
//   status.visit(smithy::Overloaded{
//       [](const PendingStatus& p) { ... },
//       [](const ReadyStatus& r) { ... },
//       [](const CancelledStatus&) { ... },
//       [](std::monostate) { ... },  // the union's empty state
//   });
template <typename... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <typename... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

}  // namespace smithy

#endif  // SMITHY_CORE_OVERLOADED_H_
