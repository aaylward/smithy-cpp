// Fuzz target: client-address derivation (ADR-0012). The walk parses two
// attacker-influenced inputs — the x-forwarded-for header and, indirectly,
// whatever ends up in peer_address — and must never crash on either; a
// derived non-empty address must itself be one the trust machinery can
// parse (it feeds policy keys). TrustedProxies construction must either
// succeed or throw std::invalid_argument, never anything else.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#include "smithy/http/forwarded.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  static const smithy::http::TrustedProxies trusted(
      {"10.0.0.0/8", "192.0.2.1", "2001:db8::/32", "::ffff:172.16.0.0/108"});

  // First line: peer_address; the rest: one x-forwarded-for value.
  smithy::http::HttpRequest request;
  const auto newline = text.find('\n');
  request.peer_address = std::string(text.substr(0, newline));
  if (newline != std::string_view::npos) {
    request.headers.Set("x-forwarded-for", std::string(text.substr(newline + 1)));
  }
  const std::string client = smithy::http::ClientAddress(request, trusted);
  if (!client.empty()) {
    // Canonical output round-trips: a derived address is always a valid
    // host-route trust entry.
    const smithy::http::TrustedProxies echo({client});
    if (!echo.Contains(client)) std::abort();
  }

  (void)trusted.Contains(text);
  try {
    const smithy::http::TrustedProxies probe({std::string(text)});
    (void)probe.Contains("10.0.0.1");
  } catch (const std::invalid_argument&) {
  }
  return 0;
}
