// The consumer-side integration test the quick start walks through: implement
// the generated handler, then drive the generated server with the generated
// client over the loopback transport and a real socket.

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "acme/todo/cbor/client.h"
#include "acme/todo/cbor/server.h"
#include "acme/todo/client.h"
#include "acme/todo/jsonrpc/client.h"
#include "acme/todo/jsonrpc/server.h"
#include "acme/todo/server.h"
#include "smithy/client/config.h"
#include "smithy/http/loopback.h"
#include "smithy/http/socket_transport.h"
#include "smithy/http/trace_context.h"
#include "smithy/server/middleware.h"

namespace {

using acme::todo::AddTaskInput;
using acme::todo::AddTaskOutput;
using acme::todo::GetTaskInput;
using acme::todo::GetTaskOutput;
using acme::todo::NoSuchTask;
using acme::todo::TodoClient;
using acme::todo::TodoHandler;
using acme::todo::TodoServer;

// [quickstart:handler] This exact block is the handler docs/quickstart.md
// teaches; QuickstartMirrorTest fails if the two ever diverge.
class InMemoryHandler final : public TodoHandler {
 public:
  smithy::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input,
                                         const smithy::server::RequestContext&) override {
    const std::lock_guard<std::mutex> lock(mu_);
    const std::string id = "task-" + std::to_string(next_id_++);
    titles_[id] = input.title;
    return AddTaskOutput{.taskId = id, .title = input.title};
  }

  smithy::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input,
                                         const smithy::server::RequestContext&) override {
    const std::lock_guard<std::mutex> lock(mu_);
    const auto it = titles_.find(input.taskId);
    if (it == titles_.end()) {
      smithy::Error error = smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
      error.set_detail(NoSuchTask{.message = "no task: " + input.taskId});
      return error;  // the server turns this into the modeled 404
    }
    return GetTaskOutput{.taskId = input.taskId, .title = it->second, .done = false};
  }

 private:
  std::mutex mu_;  // handlers must be thread-safe: transports dispatch on a thread pool
  int next_id_ = 1;
  std::map<std::string, std::string> titles_;
};
// [/quickstart:handler]

enum class Transport { kLoopback, kSocket };

class TodoIntegrationTest : public ::testing::TestWithParam<Transport> {
 protected:
  void SetUp() override {
    server_ = std::make_unique<TodoServer>(std::make_shared<InMemoryHandler>());
    smithy::ClientConfig config;
    if (GetParam() == Transport::kLoopback) {
      auto loopback = std::make_shared<smithy::http::Loopback>();
      ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
      config.http_client = loopback;
    } else {
      socket_server_ = std::make_unique<smithy::http::SocketHttpServer>();
      ASSERT_TRUE(socket_server_->Start(server_->Handler()).ok());
      config.endpoint = "http://127.0.0.1:" + std::to_string(socket_server_->port());
    }
    auto client = TodoClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<TodoClient>(std::move(*client));
  }

  void TearDown() override {
    if (socket_server_ != nullptr) socket_server_->Stop();
  }

  std::unique_ptr<TodoServer> server_;
  std::unique_ptr<smithy::http::SocketHttpServer> socket_server_;
  std::unique_ptr<TodoClient> client_;
};

TEST_P(TodoIntegrationTest, AddThenGetRoundTrips) {
  const auto added = client_->AddTask(AddTaskInput{.title = "write the quick start"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->title, "write the quick start");

  const auto fetched = client_->GetTask(GetTaskInput{.taskId = added->taskId});
  ASSERT_TRUE(fetched.ok()) << fetched.error().message();
  EXPECT_EQ(fetched->title, "write the quick start");
}

TEST_P(TodoIntegrationTest, ModeledErrorsSurfaceTyped) {
  const auto missing = client_->GetTask(GetTaskInput{.taskId = "nope"});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code(), "NoSuchTask");
  ASSERT_NE(missing.error().detail<NoSuchTask>(), nullptr);
  EXPECT_EQ(missing.error().detail<NoSuchTask>()->message, "no task: nope");
}

TEST_P(TodoIntegrationTest, ConstraintValidationRejectsBeforeTheHandler) {
  // @length(min: 1) on title: the framework answers 400 ValidationException.
  const auto rejected = client_->AddTask(AddTaskInput{.title = ""});
  ASSERT_FALSE(rejected.ok());
}

INSTANTIATE_TEST_SUITE_P(Transports, TodoIntegrationTest,
                         ::testing::Values(Transport::kLoopback, Transport::kSocket),
                         [](const auto& info) {
                           return info.param == Transport::kLoopback ? "Loopback" : "Socket";
                         });

// The same protocol-agnostic model, bound to rpcv2Cbor by a different
// `apply` overlay (model/bindings/rpcv2cbor.smithy): identical handler
// semantics over a completely different wire protocol.
TEST(TodoCborTest, SameModelServesRpcv2Cbor) {
  class CborHandler final : public acme::todo::cbor::TodoHandler {
   public:
    smithy::Outcome<acme::todo::cbor::AddTaskOutput> AddTask(
        const acme::todo::cbor::AddTaskInput& input,
        const smithy::server::RequestContext&) override {
      return acme::todo::cbor::AddTaskOutput{.taskId = "task-1", .title = input.title};
    }
    smithy::Outcome<acme::todo::cbor::GetTaskOutput> GetTask(
        const acme::todo::cbor::GetTaskInput& input,
        const smithy::server::RequestContext&) override {
      smithy::Error error = smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
      error.set_detail(acme::todo::cbor::NoSuchTask{.message = "no task: " + input.taskId});
      return error;
    }
  };

  acme::todo::cbor::TodoServer server(std::make_shared<CborHandler>());
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server.Handler()).ok());
  smithy::ClientConfig config;
  config.http_client = loopback;
  auto client = acme::todo::cbor::TodoClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  const auto added = client->AddTask(acme::todo::cbor::AddTaskInput{.title = "ship it"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->taskId, "task-1");
  EXPECT_EQ(added->title, "ship it");

  const auto missing = client->GetTask(acme::todo::cbor::GetTaskInput{.taskId = "nope"});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code(), "NoSuchTask");
}

// And a third overlay (model/bindings/jsonrpc2.smithy) binds the same model
// to JSON-RPC 2.0: a single POST / endpoint dispatching on the envelope's
// method member.
TEST(TodoJsonRpcTest, SameModelServesJsonRpc2) {
  class JsonRpcHandler final : public acme::todo::jsonrpc::TodoHandler {
   public:
    smithy::Outcome<acme::todo::jsonrpc::AddTaskOutput> AddTask(
        const acme::todo::jsonrpc::AddTaskInput& input,
        const smithy::server::RequestContext&) override {
      return acme::todo::jsonrpc::AddTaskOutput{.taskId = "task-1", .title = input.title};
    }
    smithy::Outcome<acme::todo::jsonrpc::GetTaskOutput> GetTask(
        const acme::todo::jsonrpc::GetTaskInput& input,
        const smithy::server::RequestContext&) override {
      smithy::Error error = smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
      error.set_detail(acme::todo::jsonrpc::NoSuchTask{.message = "no task: " + input.taskId});
      return error;
    }
  };

  acme::todo::jsonrpc::TodoServer server(std::make_shared<JsonRpcHandler>());
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server.Handler()).ok());
  smithy::ClientConfig config;
  config.http_client = loopback;
  auto client = acme::todo::jsonrpc::TodoClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  const auto added = client->AddTask(acme::todo::jsonrpc::AddTaskInput{.title = "ship it"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->taskId, "task-1");
  EXPECT_EQ(added->title, "ship it");

  const auto missing = client->GetTask(acme::todo::jsonrpc::GetTaskInput{.taskId = "nope"});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code(), "NoSuchTask");
  ASSERT_NE(missing.error().detail<acme::todo::jsonrpc::NoSuchTask>(), nullptr);
}

// Framework-level routing at the module boundary: requests no generated
// client would send (wrong method, unknown path) get the router's shaped
// answers — 405 with a deterministic, deduplicated Allow list, and 404.
TEST(TodoRoutingTest, WrongMethodGets405WithAllowAndUnknownPathGets404) {
  TodoServer server(std::make_shared<InMemoryHandler>());
  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server.Handler()).ok());

  smithy::http::HttpRequest wrong_method;
  wrong_method.method = "DELETE";
  wrong_method.target = "/tasks";
  const auto not_allowed = loopback->Send(wrong_method);
  ASSERT_TRUE(not_allowed.ok());
  EXPECT_EQ(not_allowed->status, 405);
  EXPECT_EQ(not_allowed->headers.Get("allow").value_or(""), "POST");

  smithy::http::HttpRequest unknown;
  unknown.method = "GET";
  unknown.target = "/no/such/route";
  const auto not_found = loopback->Send(unknown);
  ASSERT_TRUE(not_found.ok());
  EXPECT_EQ(not_found->status, 404);
}

// The handler-visible request context (ADR-0010) at the module boundary:
// the raw request — unmodeled headers, the transport-stamped peer address,
// the inbound traceparent — reaches a consumer's handler.
TEST(TodoMetadataTest, RestHandlerSeesHeadersPeerAndTraceOverARealSocket) {
  class MetadataHandler final : public TodoHandler {
   public:
    smithy::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input,
                                           const smithy::server::RequestContext& context) override {
      const auto trace =
          smithy::http::ParseTraceparent(context.request->headers.Get("traceparent").value_or(""));
      return AddTaskOutput{.taskId = context.request->peer_address,
                           .title = input.title + "|" +
                                    context.request->headers.Get("x-tenant").value_or("missing") +
                                    "|" + (trace.has_value() ? trace->trace_id : "no-trace")};
    }
    smithy::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input,
                                           const smithy::server::RequestContext&) override {
      return smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
    }
  };

  TodoServer server(std::make_shared<MetadataHandler>());
  smithy::http::SocketHttpServer transport;
  ASSERT_TRUE(transport.Start(server.Handler()).ok());

  // A raw request so unmodeled headers ride along (generated clients only
  // send what the model binds).
  smithy::http::SocketHttpClient raw("127.0.0.1", transport.port());
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/tasks";
  request.headers.Set("content-type", "application/json");
  request.headers.Set("x-tenant", "acme");
  request.headers.Set("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
  request.body = R"({"title":"trace me"})";
  const auto response = raw.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_NE(response->body.find("trace me|acme|0af7651916cd43dd8448eb211c80319c"),
            std::string::npos)
      << response->body;
  EXPECT_NE(response->body.find("127.0.0.1:"), std::string::npos) << response->body;

  // And with no inbound traceparent at all, the ingress mints one
  // (ADR-0011): the handler still sees a parseable identity, never
  // "no-trace".
  smithy::http::HttpRequest bare;
  bare.method = "POST";
  bare.target = "/tasks";
  bare.headers.Set("content-type", "application/json");
  bare.body = R"({"title":"minted"})";
  const auto minted = raw.Send(bare);
  ASSERT_TRUE(minted.ok()) << minted.error().message();
  EXPECT_EQ(minted->body.find("no-trace"), std::string::npos) << minted->body;
  EXPECT_NE(minted->body.find("minted|missing|"), std::string::npos) << minted->body;
  transport.Stop();
}

// The production middleware chain from the guide — Guard, then Observe, then
// liveness and readiness HealthEndpoints — composed around a generated server
// and driven by the generated client.
TEST(TodoMiddlewareTest, GuardObserveAndHealthComposeAroundTheServer) {
  TodoServer server(std::make_shared<InMemoryHandler>());
  int started = 0;
  int completed = 0;
  bool admit = true;
  bool ready = true;
  auto handler = smithy::server::Chain(
      {smithy::server::Guard([&admit](const smithy::http::HttpRequest&) { return admit; },
                             smithy::server::TooManyRequests(std::chrono::seconds(1))),
       // Observe takes on_complete first, then the optional on_start.
       smithy::server::Observe(
           [&completed](const smithy::server::RequestObservation&) { ++completed; },
           [&started](const smithy::server::RequestStart&) { ++started; }),
       smithy::server::HealthEndpoint(),
       smithy::server::HealthEndpoint("/readyz", {{"db", [&ready] { return ready; }}})},
      server.Handler());

  auto loopback = std::make_shared<smithy::http::Loopback>();
  ASSERT_TRUE(loopback->Start(handler).ok());

  // Liveness answers without reaching the router, and is observed.
  smithy::http::HttpRequest health;
  health.method = "GET";
  health.target = "/health";
  const auto health_response = loopback->Send(health);
  ASSERT_TRUE(health_response.ok());
  EXPECT_EQ(health_response->status, 200);
  EXPECT_EQ(health_response->body, R"({"status":"healthy"})");

  // Readiness re-probes on every request: 200 while the dependency serves,
  // 503 naming it once it stops.
  smithy::http::HttpRequest readyz;
  readyz.method = "GET";
  readyz.target = "/readyz";
  const auto ready_response = loopback->Send(readyz);
  ASSERT_TRUE(ready_response.ok());
  EXPECT_EQ(ready_response->status, 200);
  ready = false;
  const auto unready_response = loopback->Send(readyz);
  ASSERT_TRUE(unready_response.ok());
  EXPECT_EQ(unready_response->status, 503);
  EXPECT_EQ(unready_response->body, R"({"status":"unhealthy","failing":["db"]})");

  // The generated client works through the chain.
  smithy::ClientConfig config;
  config.http_client = loopback;
  auto created = TodoClient::Create(std::move(config));
  ASSERT_TRUE(created.ok()) << created.error().message();
  TodoClient client = std::move(*created);
  const auto added = client.AddTask(AddTaskInput{.title = "compose middleware"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->title, "compose middleware");

  // Once admit flips, Guard sheds load with the shaped 429 before Observe.
  admit = false;
  smithy::http::HttpRequest denied;
  denied.method = "POST";
  denied.target = "/tasks";
  const auto denied_response = loopback->Send(denied);
  ASSERT_TRUE(denied_response.ok());
  EXPECT_EQ(denied_response->status, 429);
  EXPECT_EQ(denied_response->headers.Get("retry-after").value_or(""), "1");

  // health + two readyz + AddTask; the rejected request never reached Observe.
  EXPECT_EQ(started, 4);
  EXPECT_EQ(completed, 4);
}

}  // namespace
