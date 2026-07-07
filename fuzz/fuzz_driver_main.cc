// Standalone driver so every fuzz target also runs as an ordinary test:
// deterministic pseudo-random inputs (xorshift) of varied sizes, plus edge
// sizes. Real fuzzing links libFuzzer instead (bazel --config=fuzz).
#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main() {
  LLVMFuzzerTestOneInput(nullptr, 0);
  std::uint64_t state = 0x9E3779B97F4A7C15ULL;
  auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  std::vector<std::uint8_t> buffer;
  for (int iteration = 0; iteration < 20000; ++iteration) {
    buffer.resize(next() % 300);
    for (auto& byte : buffer) byte = static_cast<std::uint8_t>(next());
    // Bias toward text-ish inputs half the time so parsers get past byte one.
    if (iteration % 2 == 0) {
      for (auto& byte : buffer) byte = static_cast<std::uint8_t>(' ' + byte % 95);
    }
    LLVMFuzzerTestOneInput(buffer.data(), buffer.size());
  }
  return 0;
}
