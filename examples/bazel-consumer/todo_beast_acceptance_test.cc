// Production-transport acceptance from the consumer's side of the module
// boundary: the generated Todo service on BeastServerTransport, driven by the
// generated client over BeastHttpClient::FromConfig — the exact wiring
// docs/production-guide.md teaches. Positive: the handler-executor knob is
// consumable and blocked handlers cannot starve the wire. Negative: a
// throwing handler is contained as a correlated 500 and the service survives.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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

// A handler with production-shaped behaviors on demand: "block" waits until
// kBarrier handlers are blocked together (provably concurrent dispatch),
// "boom" throws (the bug every real service eventually ships).
class AcceptanceHandler final : public TodoHandler {
 public:
  static constexpr int kBarrier = 2;

  smithy::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input) override {
    if (input.title == "boom") {
      throw std::runtime_error("consumer handler bug");
    }
    if (input.title == "block") {
      ++blocked_;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (blocked_.load() < kBarrier && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      const bool together = blocked_.load() >= kBarrier;
      return AddTaskOutput{.taskId = "task-blocked", .title = together ? "ok" : "starved"};
    }
    return AddTaskOutput{.taskId = "task-1", .title = input.title};
  }

  smithy::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input) override {
    smithy::Error error = smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
    error.set_detail(NoSuchTask{.message = "no task: " + input.taskId});
    return error;
  }

 private:
  std::atomic<int> blocked_{0};
};

// Starts the service on the production transport and returns a generated
// client wired through BeastHttpClient::FromConfig (the production-guide
// flow). threads = 1 makes the executor observable: without it, one blocked
// handler would freeze the whole wire.
class TodoBeastAcceptanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    server_ = std::make_unique<TodoServer>(std::make_shared<AcceptanceHandler>());
    transport_ = std::make_unique<smithy::http::BeastServerTransport>(
        smithy::http::BeastServerTransport::Options{.threads = 1});
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

TEST_F(TodoBeastAcceptanceTest, BlockedHandlersDoNotStarveTheServer) {
  // Both calls block inside the handler until they see each other — only
  // possible when handlers run off the io thread (handler_threads default),
  // since this server has a single io thread.
  std::thread first([&] {
    const auto blocked = client_->AddTask(AddTaskInput{.title = "block"});
    ASSERT_TRUE(blocked.ok()) << blocked.error().message();
    EXPECT_EQ(blocked->title, "ok");
  });
  const auto second = client_->AddTask(AddTaskInput{.title = "block"});
  first.join();
  ASSERT_TRUE(second.ok()) << second.error().message();
  EXPECT_EQ(second->title, "ok");
}

TEST_F(TodoBeastAcceptanceTest, HandlerExceptionIsContainedAndServiceSurvives) {
  // The negative path every service hits eventually: a handler throws. The
  // framework answers a correlated 500 instead of dying (issue #41), and the
  // very next call on the same server succeeds.
  const auto boom = client_->AddTask(AddTaskInput{.title = "boom"});
  ASSERT_FALSE(boom.ok());

  const auto after = client_->AddTask(AddTaskInput{.title = "still serving"});
  ASSERT_TRUE(after.ok()) << after.error().message();
  EXPECT_EQ(after->title, "still serving");
}

}  // namespace
