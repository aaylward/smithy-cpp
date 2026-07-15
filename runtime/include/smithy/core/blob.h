#ifndef SMITHY_CORE_BLOB_H_
#define SMITHY_CORE_BLOB_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smithy {

// Owning byte buffer for Smithy blob shapes. Distinct from std::string to keep
// binary payloads and text from mixing accidentally.
class Blob {
 public:
  Blob() = default;
  explicit Blob(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

  static Blob FromString(std::string_view text) {
    return Blob(std::vector<std::uint8_t>(text.begin(), text.end()));
  }

  std::string ToString() const { return std::string(bytes_.begin(), bytes_.end()); }

  const std::vector<std::uint8_t>& bytes() const { return bytes_; }
  std::vector<std::uint8_t>& bytes() { return bytes_; }
  const std::uint8_t* data() const { return bytes_.data(); }
  std::size_t size() const { return bytes_.size(); }
  bool empty() const { return bytes_.empty(); }

  friend bool operator==(const Blob& a, const Blob& b) { return a.bytes_ == b.bytes_; }
  // Lexicographic by bytes, so blob-bearing generated structs stay orderable.
  friend auto operator<=>(const Blob& a, const Blob& b) { return a.bytes_ <=> b.bytes_; }

 private:
  std::vector<std::uint8_t> bytes_;
};

}  // namespace smithy

// Hashes by byte content, so blob-bearing generated structs can key unordered
// containers (issue #49). Process-local: never persist hash values.
template <>
struct std::hash<smithy::Blob> {
  std::size_t operator()(const smithy::Blob& blob) const noexcept {
    return std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
  }
};

#endif  // SMITHY_CORE_BLOB_H_
