// Production-transport acceptance from the consumer's side of the module
// boundary: the generated Todo service on BeastServerTransport, driven by the
// generated client over BeastHttpClient::FromConfig — the exact wiring
// docs/production-guide.md teaches. What this proves is the boundary itself:
// the hardening knobs (handler_threads among them) are visible and usable
// from a consumer build, generated round trips and modeled errors survive the
// production transport, and a throwing handler is contained as a 500 without
// killing the service. The executor's concurrency semantics are pinned where
// they live, in the runtime suite (beast_transport_test.cc).

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include "acme/todo/client.h"
#include "acme/todo/server.h"
#include "smithy/client/config.h"
#include "smithy/http/beast_transport.h"

namespace {

using acme::todo::AddTaskInput;
using acme::todo::AddTaskOutput;
using acme::todo::GetTaskInput;
using acme::todo::GetTaskOutput;
using acme::todo::NoSuchTask;
using acme::todo::TodoClient;
using acme::todo::TodoHandler;
using acme::todo::TodoServer;

// A handler with the negative path every real service eventually ships: a
// "boom" title throws (the bug the framework must contain).
class AcceptanceHandler final : public TodoHandler {
 public:
  smithy::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input) override {
    if (input.title == "boom") {
      throw std::runtime_error("consumer handler bug");
    }
    return AddTaskOutput{.taskId = "task-1", .title = input.title};
  }

  smithy::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input) override {
    smithy::Error error = smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
    error.set_detail(NoSuchTask{.message = "no task: " + input.taskId});
    return error;
  }
};

// Starts the service on the production transport and returns a generated
// client wired through BeastHttpClient::FromConfig (the production-guide
// flow). Setting handler_threads here is the consumer-facing proof of the
// executor knob: the field must exist and compose across the module boundary.
class TodoBeastAcceptanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    server_ = std::make_unique<TodoServer>(std::make_shared<AcceptanceHandler>());
    transport_ = std::make_unique<smithy::http::BeastServerTransport>(
        smithy::http::BeastServerTransport::Options{
            .threads = 1, .handler_threads = 8, .drain_timeout_seconds = 5});
    ASSERT_TRUE(transport_->Start(server_->Handler()).ok());

    smithy::ClientConfig config;
    config.endpoint = "http://127.0.0.1:" + std::to_string(transport_->port());
    auto http_client = smithy::http::BeastHttpClient::FromConfig(config);
    ASSERT_TRUE(http_client.ok()) << http_client.error().message();
    config.http_client = *http_client;
    auto client = TodoClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<TodoClient>(std::move(*client));
  }

  void TearDown() override { transport_->Stop(); }

  std::unique_ptr<TodoServer> server_;
  std::unique_ptr<smithy::http::BeastServerTransport> transport_;
  std::unique_ptr<TodoClient> client_;
};

TEST_F(TodoBeastAcceptanceTest, RoundTripsAndModeledErrorsWork) {
  const auto added = client_->AddTask(AddTaskInput{.title = "ship on beast"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->title, "ship on beast");

  const auto missing = client_->GetTask(GetTaskInput{.taskId = "nope"});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code(), "NoSuchTask");
  ASSERT_NE(missing.error().detail<NoSuchTask>(), nullptr);
}

TEST_F(TodoBeastAcceptanceTest, LifecycleStopsAndRestartsAcrossTransportGenerations) {
  // The rolling-restart pattern from the production guide, composed entirely
  // from the consumer-visible API: serve, Stop() (bounded — the drain knob
  // above is the consumer's contract), then a fresh transport generation on
  // the same generated server object serves again.
  const auto first = client_->AddTask(AddTaskInput{.title = "before restart"});
  ASSERT_TRUE(first.ok()) << first.error().message();

  const auto begin = std::chrono::steady_clock::now();
  transport_->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(10));

  smithy::http::BeastServerTransport second_generation(
      smithy::http::BeastServerTransport::Options{.threads = 1, .handler_threads = 8});
  ASSERT_TRUE(second_generation.Start(server_->Handler()).ok());
  smithy::ClientConfig config;
  config.endpoint = "http://127.0.0.1:" + std::to_string(second_generation.port());
  auto http_client = smithy::http::BeastHttpClient::FromConfig(config);
  ASSERT_TRUE(http_client.ok()) << http_client.error().message();
  config.http_client = *http_client;
  auto client = TodoClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();
  const auto after = client->AddTask(AddTaskInput{.title = "after restart"});
  ASSERT_TRUE(after.ok()) << after.error().message();
  EXPECT_EQ(after->title, "after restart");
  second_generation.Stop();
}

TEST_F(TodoBeastAcceptanceTest, HandlerExceptionIsContainedAndServiceSurvives) {
  // The framework answers a correlated 500 instead of dying (issue #41), and
  // the very next call on the same server succeeds.
  const auto boom = client_->AddTask(AddTaskInput{.title = "boom"});
  ASSERT_FALSE(boom.ok());

  const auto after = client_->AddTask(AddTaskInput{.title = "still serving"});
  ASSERT_TRUE(after.ok()) << after.error().message();
  EXPECT_EQ(after->title, "still serving");
}

}  // namespace
