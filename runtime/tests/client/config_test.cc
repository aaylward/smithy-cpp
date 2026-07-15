// smithy::ClientConfig: the production transport knobs live on the config
// (issue #49) — TLS verification/trust and pool sizing beside the timeout —
// so configuring a client is one struct, not a second transport-specific
// Options object. Transports built from the config (e.g.
// BeastHttpClient::FromConfig) honor every one of these.

#include "smithy/client/config.h"

#include <gtest/gtest.h>

namespace smithy {
namespace {

TEST(ClientConfigTest, TransportKnobsLiveOnTheConfig) {
  const ClientConfig config;
  // TLS: certificate + hostname verification on by default; ca_pem replaces
  // the system trust roots (private CAs, tests).
  EXPECT_TRUE(config.tls.verify_peer);
  EXPECT_TRUE(config.tls.ca_pem.empty());
  // Idle keep-alive connections a pooling transport retains for reuse.
  EXPECT_EQ(config.max_idle_connections, 4u);
  // One timeout, owned by the config; FromConfig-built transports copy it.
  EXPECT_EQ(config.request_timeout_ms, 30000);
}

}  // namespace
}  // namespace smithy
