#ifndef SMITHY_CLIENT_CONFIG_H_
#define SMITHY_CLIENT_CONFIG_H_

#include <memory>
#include <string>

#include "smithy/http/transport.h"

namespace smithy {

// Configuration shared by every generated client.
//
//   smithy::ClientConfig cfg;
//   cfg.endpoint = "http://localhost:8080";
//   WeatherClient client(cfg);
//
// When http_client is left null, generated clients construct the built-in
// socket transport from the endpoint. Tests inject a Loopback or mock here.
struct ClientConfig {
  std::string endpoint;
  int request_timeout_ms = 30000;
  std::string user_agent = "smithy-cpp/0.0.0-dev";

  // Optional transport override; shared so several clients can reuse one.
  std::shared_ptr<http::HttpClient> http_client;
};

}  // namespace smithy

#endif  // SMITHY_CLIENT_CONFIG_H_
