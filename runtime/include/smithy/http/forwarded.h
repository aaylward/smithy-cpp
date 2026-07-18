#ifndef SMITHY_HTTP_FORWARDED_H_
#define SMITHY_HTTP_FORWARDED_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "smithy/http/message.h"

namespace smithy::http {

// The deployment's reverse-proxy trust boundary (ADR-0012): the set of
// addresses whose x-forwarded-for entries count during client-address
// derivation. Built from CIDR strings ("10.0.0.0/8", "2600:1f00::/24"); a
// bare address is a host route. Construction throws std::invalid_argument
// on a malformed entry — a misconfigured trust boundary must fail
// deployment, not silently widen or narrow. A copyable value; Contains is
// const and safe to share across threads.
//
// "Trust nothing" is a statement, not an absence (issue #104): None() is
// the greppable claim that the service is directly reachable — ClientAddress
// then reduces every request to its bare canonical peer. There is
// deliberately no default constructor, because behind a proxy the
// accidental empty set (an unset config value) silently collapses all
// traffic onto the proxy's one policy key.
class TrustedProxies {
 public:
  static TrustedProxies None();
  explicit TrustedProxies(const std::vector<std::string>& cidrs);

  // Whether a bare numeric address (no port, no brackets) is inside any
  // trusted network. IPv4-mapped IPv6 matches as the embedded IPv4.
  // Unparseable input is in no network.
  bool Contains(std::string_view address) const;

 private:
  TrustedProxies() = default;

  friend std::string ClientAddress(const HttpRequest& request, const TrustedProxies& trusted);

  // The parsed-form check ClientAddress's walk uses (one parse per entry,
  // not two): 4 (AF_INET) or 16 (AF_INET6) significant bytes.
  bool ContainsBytes(const std::array<std::uint8_t, 16>& bytes, int family) const;

  struct Network {
    std::array<std::uint8_t, 16> bytes{};
    int family = 0;
    int prefix_bits = 0;
  };
  std::vector<Network> networks_;
};

// The client's address as derived from the L4 peer and x-forwarded-for
// (ADR-0012), in canonical numeric form (no port, no brackets, no v6 zone
// id; IPv4-mapped IPv6 as the embedded IPv4) — directly usable as a policy
// or metrics key.
//
// The walk is anchored at request.peer_address, the one fact a client
// cannot forge: a peer outside the trust set IS the client and the header
// is ignored wholly. A trusted peer walks the entries right to left
// (rightmost were appended last, by the proxies closest to us), skipping
// trusted entries; the first untrusted entry is the client. Entries
// tolerate the forms real proxies emit — "ip", "ip:port", "[v6]",
// "[v6]:port", a zone id dropped — and multiple x-forwarded-for headers
// join in order (RFC 9110 list semantics). A chain exhausted with
// everything trusted yields the leftmost entry (the request originated
// inside the trusted tier); a malformed entry ("unknown", an obfuscated
// token, garbage) stops the walk at the last vetted position. An empty or
// unparseable peer_address (Loopback, handler chains driven directly in
// tests) derives "".
//
// Caveat, inherent to the recursive walk (nginx real_ip_recursive shares
// it): a client whose own address falls inside the trust set is skipped as
// a hop and can forge its ancestry — trust only networks that proxies
// alone occupy.
std::string ClientAddress(const HttpRequest& request, const TrustedProxies& trusted);

}  // namespace smithy::http

#endif  // SMITHY_HTTP_FORWARDED_H_
