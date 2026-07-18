// Pins ADR-0012: client-address derivation anchored at the L4 peer, with
// the rightmost-untrusted walk over x-forwarded-for. The suite's job is the
// security edges — every way an attacker-authored header must lose to the
// one fact a client cannot forge.

#include "smithy/http/forwarded.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace smithy::http {
namespace {

HttpRequest RequestFrom(const std::string& peer, const std::vector<std::string>& xff = {}) {
  HttpRequest request;
  request.peer_address = peer;
  for (const std::string& value : xff) {
    request.headers.Add("x-forwarded-for", value);
  }
  return request;
}

TEST(TrustedProxiesTest, MatchesV4AndV6Cidrs) {
  const TrustedProxies trusted({"10.0.0.0/8", "2001:db8::/32"});
  EXPECT_TRUE(trusted.Contains("10.0.0.1"));
  EXPECT_TRUE(trusted.Contains("10.255.255.255"));
  EXPECT_FALSE(trusted.Contains("11.0.0.1"));
  EXPECT_TRUE(trusted.Contains("2001:db8::1"));
  EXPECT_TRUE(trusted.Contains("2001:db8:ffff::1"));
  EXPECT_FALSE(trusted.Contains("2001:db9::1"));
}

TEST(TrustedProxiesTest, MatchesMidBytePrefixBoundaries) {
  // /20 splits the third octet: 172.16.0.0–172.16.15.255.
  const TrustedProxies trusted({"172.16.0.0/20"});
  EXPECT_TRUE(trusted.Contains("172.16.15.255"));
  EXPECT_FALSE(trusted.Contains("172.16.16.0"));
}

TEST(TrustedProxiesTest, ABareAddressIsAHostRoute) {
  const TrustedProxies trusted({"192.0.2.7", "2001:db8::7"});
  EXPECT_TRUE(trusted.Contains("192.0.2.7"));
  EXPECT_FALSE(trusted.Contains("192.0.2.8"));
  EXPECT_TRUE(trusted.Contains("2001:db8::7"));
  EXPECT_FALSE(trusted.Contains("2001:db8::8"));
}

TEST(TrustedProxiesTest, AZeroPrefixTrustsTheWholeFamily) {
  const TrustedProxies v4_all({"0.0.0.0/0"});
  EXPECT_TRUE(v4_all.Contains("203.0.113.9"));
  EXPECT_FALSE(v4_all.Contains("2001:db8::1"));  // families never cross
}

TEST(TrustedProxiesTest, V4MappedV6IsTheEmbeddedV4Everywhere) {
  // A dual-stack listener reports ::ffff:10.0.0.1; a v4 CIDR must match it,
  // and a CIDR written in mapped form must match plain v4 (prefix
  // translated across the /96 mapping range).
  const TrustedProxies v4({"10.0.0.0/8"});
  EXPECT_TRUE(v4.Contains("::ffff:10.0.0.1"));
  EXPECT_FALSE(v4.Contains("::ffff:11.0.0.1"));
  const TrustedProxies mapped({"::ffff:10.0.0.0/104"});
  EXPECT_TRUE(mapped.Contains("10.1.2.3"));
  EXPECT_FALSE(mapped.Contains("11.0.0.1"));
}

TEST(TrustedProxiesTest, TheDefaultAndTheUnparseableTrustNothing) {
  const TrustedProxies nothing;
  EXPECT_FALSE(nothing.Contains("127.0.0.1"));
  const TrustedProxies trusted({"10.0.0.0/8"});
  EXPECT_FALSE(trusted.Contains(""));
  EXPECT_FALSE(trusted.Contains("not-an-ip"));
  EXPECT_FALSE(trusted.Contains("10.0.0.1:443"));  // bare addresses only
}

TEST(TrustedProxiesTest, MalformedConfigurationThrows) {
  // A trust boundary that does not parse must fail deployment (ADR-0012),
  // not silently widen or narrow.
  EXPECT_THROW(TrustedProxies({"10.0.0.0/33"}), std::invalid_argument);
  EXPECT_THROW(TrustedProxies({"2001:db8::/129"}), std::invalid_argument);
  EXPECT_THROW(TrustedProxies({"10.0.0.0/"}), std::invalid_argument);
  EXPECT_THROW(TrustedProxies({"10.0.0.0/8/8"}), std::invalid_argument);
  EXPECT_THROW(TrustedProxies({"10.0.0.0/ 8"}), std::invalid_argument);
  EXPECT_THROW(TrustedProxies({"gateway/8"}), std::invalid_argument);
  EXPECT_THROW(TrustedProxies({""}), std::invalid_argument);
  // A mapped base with a prefix spanning non-IPv4 space is meaningless as a
  // proxy trust boundary.
  EXPECT_THROW(TrustedProxies({"::ffff:10.0.0.0/64"}), std::invalid_argument);
}

TEST(ClientAddressTest, AnUntrustedPeerIsTheClientAndTheHeaderIsIgnored) {
  // The direct-connect spoof: whatever the client wrote into
  // x-forwarded-for is noise when the TCP peer itself is not a proxy.
  const TrustedProxies trusted({"10.0.0.0/8"});
  const auto request = RequestFrom("203.0.113.9:52814", {"198.51.100.7, 10.0.0.1"});
  EXPECT_EQ(ClientAddress(request, trusted), "203.0.113.9");
}

TEST(ClientAddressTest, ATrustedPeerYieldsTheRightmostUntrustedEntry) {
  const TrustedProxies trusted({"10.0.0.0/8"});
  const auto request = RequestFrom("10.0.0.1:443", {"203.0.113.9"});
  EXPECT_EQ(ClientAddress(request, trusted), "203.0.113.9");
}

TEST(ClientAddressTest, SpoofedPrefixEntriesNeverWin) {
  // The client sent its own x-forwarded-for; the edge proxy appended the
  // real address after it. The walk stops at the appended entry — the
  // attacker-authored prefix is unreachable.
  const TrustedProxies trusted({"10.0.0.0/8"});
  const auto request = RequestFrom("10.0.0.1:443", {"198.51.100.7, 203.0.113.9"});
  EXPECT_EQ(ClientAddress(request, trusted), "203.0.113.9");
}

TEST(ClientAddressTest, AChainOfTrustedTiersSkipsToTheFirstUntrusted) {
  // LB -> internal proxy -> service: both tiers trusted, the entry before
  // them is the client.
  const TrustedProxies trusted({"10.0.0.0/8", "192.168.0.0/16"});
  const auto request = RequestFrom("10.0.0.1:443", {"198.51.100.7, 203.0.113.9, 192.168.1.1"});
  EXPECT_EQ(ClientAddress(request, trusted), "203.0.113.9");
}

TEST(ClientAddressTest, AnExhaustedAllTrustedChainYieldsTheLeftmostEntry) {
  // The request originated inside the trusted tier itself.
  const TrustedProxies trusted({"10.0.0.0/8"});
  const auto request = RequestFrom("10.0.0.1:443", {"10.0.0.2, 10.0.0.3"});
  EXPECT_EQ(ClientAddress(request, trusted), "10.0.0.2");
}

TEST(ClientAddressTest, ATrustedPeerWithNoHeaderIsTheClient) {
  const TrustedProxies trusted({"10.0.0.0/8"});
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443"), trusted), "10.0.0.1");
}

TEST(ClientAddressTest, AMalformedEntryStopsTheWalkAtTheLastVettedPosition) {
  // Garbage is never trusted and never reached past: the derivation falls
  // back to the nearest vetted hop, not the junk and not what lies beyond.
  const TrustedProxies trusted({"10.0.0.0/8"});
  const auto beyond = RequestFrom("10.0.0.1:443", {"203.0.113.9, unknown, 10.0.0.2"});
  EXPECT_EQ(ClientAddress(beyond, trusted), "10.0.0.2");
  const auto immediate = RequestFrom("10.0.0.1:443", {"not-an-ip"});
  EXPECT_EQ(ClientAddress(immediate, trusted), "10.0.0.1");
  const auto empty_element = RequestFrom("10.0.0.1:443", {"203.0.113.9,,10.0.0.2"});
  EXPECT_EQ(ClientAddress(empty_element, trusted), "10.0.0.2");
}

TEST(ClientAddressTest, EntriesTolerateTheFormsRealProxiesEmit) {
  const TrustedProxies trusted({"10.0.0.0/8"});
  // Ports, whitespace, v6 brackets, bare v6 — each reduced to the bare
  // canonical address.
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {" 203.0.113.9:52814 "}), trusted),
            "203.0.113.9");
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"[2001:db8::1]:443"}), trusted),
            "2001:db8::1");
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"2001:DB8::1"}), trusted), "2001:db8::1");
  // A bracketed form with trailing junk is malformed, not half-parsed.
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"[2001:db8::1]x"}), trusted), "10.0.0.1");
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"203.0.113.9:port"}), trusted), "10.0.0.1");
}

TEST(ClientAddressTest, MultipleHeadersJoinInOrder) {
  // Proxies may add a new x-forwarded-for header instead of appending to
  // the existing one; RFC 9110 list semantics make that equivalent.
  const TrustedProxies trusted({"10.0.0.0/8"});
  const auto request = RequestFrom("10.0.0.1:443", {"198.51.100.7", "203.0.113.9, 10.0.0.2"});
  EXPECT_EQ(ClientAddress(request, trusted), "203.0.113.9");
}

TEST(ClientAddressTest, V6PeersAndMappedPeersDeriveBareCanonicalAddresses) {
  const TrustedProxies trusted({"2001:db8::/32"});
  // The transport stamps "[v6]:port"; the derivation strips to the bare
  // address, and a trusted v6 peer walks the header like any other.
  EXPECT_EQ(ClientAddress(RequestFrom("[2001:db8::1]:443", {"203.0.113.9"}), trusted),
            "203.0.113.9");
  EXPECT_EQ(ClientAddress(RequestFrom("[2001:db9::1]:443", {"203.0.113.9"}), trusted),
            "2001:db9::1");
  // A dual-stack listener's mapped peer matches v4 trust and derives v4.
  const TrustedProxies v4({"10.0.0.0/8"});
  EXPECT_EQ(ClientAddress(RequestFrom("[::ffff:10.0.0.1]:443", {"203.0.113.9"}), v4),
            "203.0.113.9");
  EXPECT_EQ(ClientAddress(RequestFrom("[::ffff:203.0.113.9]:52814"), v4), "203.0.113.9");
}

TEST(ClientAddressTest, AnEmptyPeerDerivesEmpty) {
  // Loopback and handler chains driven directly in tests have no peer;
  // nothing is known, and empty never matches a trust set — so a spoofed
  // header cannot conjure an identity where the transport reported none.
  const TrustedProxies trusted({"10.0.0.0/8", "0.0.0.0/0"});
  EXPECT_EQ(ClientAddress(RequestFrom("", {"203.0.113.9"}), trusted), "");
  EXPECT_EQ(ClientAddress(RequestFrom("garbage-peer", {"203.0.113.9"}), trusted), "");
}

}  // namespace
}  // namespace smithy::http
