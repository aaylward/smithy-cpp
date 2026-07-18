#include "smithy/http/forwarded.h"

#include <arpa/inet.h>

#include <algorithm>
#include <optional>
#include <ranges>
#include <stdexcept>

#include "smithy/http/headers.h"

namespace smithy::http {
namespace {

// A parsed numeric address: 4 (AF_INET) or 16 (AF_INET6) significant bytes.
// IPv4-mapped IPv6 is normalized to the embedded IPv4 on parse, so
// ::ffff:203.0.113.9 and 203.0.113.9 are one address everywhere — as a
// candidate, in an entry, or as a network base (ADR-0012).
struct Address {
  std::array<std::uint8_t, 16> bytes{};
  int family = 0;
};

constexpr std::array<std::uint8_t, 12> kV4MappedPrefix = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

std::optional<Address> ParseAddress(std::string_view text) {
  if (text.empty() || text.size() >= INET6_ADDRSTRLEN) {
    return std::nullopt;
  }
  const std::string copy(text);  // inet_pton needs a terminator
  Address address;
  if (inet_pton(AF_INET, copy.c_str(), address.bytes.data()) == 1) {
    address.family = AF_INET;
    return address;
  }
  if (inet_pton(AF_INET6, copy.c_str(), address.bytes.data()) != 1) {
    return std::nullopt;
  }
  address.family = AF_INET6;
  if (std::equal(kV4MappedPrefix.begin(), kV4MappedPrefix.end(), address.bytes.begin())) {
    const std::array<std::uint8_t, 16> mapped = address.bytes;
    address.bytes.fill(0);
    std::copy(mapped.begin() + 12, mapped.end(), address.bytes.begin());
    address.family = AF_INET;
  }
  return address;
}

// Canonical numeric text for an address ParseAddress produced.
std::string FormatAddress(const Address& address) {
  std::array<char, INET6_ADDRSTRLEN> text{};
  if (inet_ntop(address.family, address.bytes.data(), text.data(), text.size()) == nullptr) {
    return {};
  }
  return text.data();
}

bool AllDigits(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// One x-forwarded-for element — or a peer_address, which uses the same
// forms — reduced to the bare address: "[v6]"/"[v6]:port" unwrapped,
// "v4:port" split (two or more colons is bare IPv6). Surrounding whitespace
// was already trimmed by SplitHeaderListValues. nullopt for anything else:
// "unknown", RFC 7239 obfuscated tokens, empty elements, garbage.
std::optional<Address> ParseForwardedEntry(std::string_view entry) {
  if (entry.empty()) {
    return std::nullopt;
  }
  if (entry.front() == '[') {
    const auto close = entry.find(']');
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view port = entry.substr(close + 1);
    if (!port.empty() && (port.front() != ':' || !AllDigits(port.substr(1)))) {
      return std::nullopt;
    }
    return ParseAddress(entry.substr(1, close - 1));
  }
  const auto colons = std::count(entry.begin(), entry.end(), ':');
  if (colons == 1) {
    const auto colon = entry.find(':');
    if (!AllDigits(entry.substr(colon + 1))) {
      return std::nullopt;
    }
    entry = entry.substr(0, colon);
  }
  return ParseAddress(entry);
}

bool PrefixMatch(const std::array<std::uint8_t, 16>& network,
                 const std::array<std::uint8_t, 16>& address, int prefix_bits) {
  const int full_bytes = prefix_bits / 8;
  if (!std::equal(network.begin(), network.begin() + full_bytes, address.begin())) {
    return false;
  }
  const int remainder = prefix_bits % 8;
  if (remainder == 0) {
    return true;
  }
  const auto mask = static_cast<std::uint8_t>(0xff << (8 - remainder));
  return (network[full_bytes] & mask) == (address[full_bytes] & mask);
}

}  // namespace

TrustedProxies::TrustedProxies(const std::vector<std::string>& cidrs) {
  networks_.reserve(cidrs.size());
  for (const std::string& cidr : cidrs) {
    const auto slash = cidr.find('/');
    const auto base = ParseAddress(std::string_view(cidr).substr(0, slash));
    if (!base.has_value()) {
      throw std::invalid_argument("TrustedProxies: unparseable address in \"" + cidr + "\"");
    }
    const int max_bits = base->family == AF_INET ? 32 : 128;
    int prefix_bits = max_bits;
    if (slash != std::string::npos) {
      const std::string_view digits = std::string_view(cidr).substr(slash + 1);
      if (!AllDigits(digits) || digits.size() > 3) {
        throw std::invalid_argument("TrustedProxies: malformed prefix in \"" + cidr + "\"");
      }
      prefix_bits = 0;
      for (const char c : digits) {
        prefix_bits = prefix_bits * 10 + (c - '0');
      }
      // A base written as IPv4-mapped IPv6 was normalized to the embedded
      // IPv4, so its prefix translates too; a mapped prefix shorter than the
      // /96 mapping range would span non-IPv4 space — meaningless as a
      // proxy trust boundary, so it is a configuration error.
      const bool was_mapped = base->family == AF_INET && cidr.find(':') != std::string::npos;
      if (was_mapped) {
        if (prefix_bits < 96 || prefix_bits > 128) {
          throw std::invalid_argument("TrustedProxies: prefix outside the IPv4-mapped range in \"" +
                                      cidr + "\"");
        }
        prefix_bits -= 96;
      } else if (prefix_bits > max_bits) {
        throw std::invalid_argument("TrustedProxies: prefix too long in \"" + cidr + "\"");
      }
    }
    networks_.push_back(Network{base->bytes, base->family, prefix_bits});
  }
}

bool TrustedProxies::Contains(std::string_view address) const {
  const auto parsed = ParseAddress(address);
  return parsed.has_value() && ContainsBytes(parsed->bytes, parsed->family);
}

bool TrustedProxies::ContainsBytes(const std::array<std::uint8_t, 16>& bytes, int family) const {
  return std::any_of(networks_.begin(), networks_.end(), [&](const Network& network) {
    return network.family == family && PrefixMatch(network.bytes, bytes, network.prefix_bits);
  });
}

std::string ClientAddress(const HttpRequest& request, const TrustedProxies& trusted) {
  const auto peer = ParseForwardedEntry(request.peer_address);
  if (!peer.has_value()) {
    return {};
  }
  Address client = *peer;
  if (!trusted.ContainsBytes(client.bytes, client.family)) {
    return FormatAddress(client);
  }
  // Entries in order across headers (RFC 9110 list semantics), walked right
  // to left: rightmost were appended last, by the proxies closest to us. A
  // valid untrusted entry is the client; a malformed one stops the walk at
  // the last vetted position — garbage is never trusted and never reached
  // past. Exhausting the chain leaves the leftmost entry: the request
  // originated inside the trusted tier.
  std::vector<std::string> entries;
  for (const std::string& value : request.headers.GetAll("x-forwarded-for")) {
    for (std::string& element : SplitHeaderListValues(value)) {
      entries.push_back(std::move(element));
    }
  }
  for (const std::string& element : std::views::reverse(entries)) {
    const auto entry = ParseForwardedEntry(element);
    if (!entry.has_value()) {
      break;
    }
    client = *entry;
    if (!trusted.ContainsBytes(client.bytes, client.family)) {
      break;
    }
  }
  return FormatAddress(client);
}

}  // namespace smithy::http
