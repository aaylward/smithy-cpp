#ifndef SMITHY_CORE_HASH_H_
#define SMITHY_CORE_HASH_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <type_traits>
#include <vector>

namespace smithy {

// Hashing support for generated types (issue #49): a generated type
// specializes std::hash exactly when it defaults operator<=>, so it keys
// std::unordered_map/std::unordered_set the same way it keys std::map.
//
// Hash values are process-local. They build on std::hash, whose values the
// standard lets vary per implementation (and per run, with hardened
// libraries) — never persist a hash or compare hashes across processes.

// Mixes `value` into `seed`, order-sensitively (splitmix64 finalizer).
inline std::size_t HashCombine(std::size_t seed, std::size_t value) {
  std::uint64_t x =
      static_cast<std::uint64_t>(seed) + 0x9e3779b97f4a7c15ULL + static_cast<std::uint64_t>(value);
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return static_cast<std::size_t>(x ^ (x >> 31));
}

namespace internal {

template <typename T>
struct IsVector : std::false_type {};
template <typename E, typename A>
struct IsVector<std::vector<E, A>> : std::true_type {};

template <typename T>
struct IsMap : std::false_type {};
template <typename K, typename V, typename C, typename A>
struct IsMap<std::map<K, V, C, A>> : std::true_type {};

template <typename T>
struct IsOptional : std::false_type {};
template <typename E>
struct IsOptional<std::optional<E>> : std::true_type {};

}  // namespace internal

// Hash of one member value, as generated std::hash specializations compute
// it: the containers generated members use (std::vector, std::map, and
// std::optional around either) hash element-wise — std::hash has no
// specializations for them — and everything else defers to std::hash.
template <typename T>
std::size_t HashValue(const T& value) noexcept {
  if constexpr (internal::IsVector<T>::value) {
    std::size_t seed = value.size();
    for (const auto& element : value) {
      seed = HashCombine(seed, HashValue(element));
    }
    return seed;
  } else if constexpr (internal::IsMap<T>::value) {
    std::size_t seed = value.size();
    for (const auto& [key, mapped] : value) {
      seed = HashCombine(HashCombine(seed, HashValue(key)), HashValue(mapped));
    }
    return seed;
  } else if constexpr (internal::IsOptional<T>::value) {
    return value.has_value() ? HashCombine(1, HashValue(*value)) : 0;
  } else {
    return std::hash<T>{}(value);
  }
}

}  // namespace smithy

#endif  // SMITHY_CORE_HASH_H_
