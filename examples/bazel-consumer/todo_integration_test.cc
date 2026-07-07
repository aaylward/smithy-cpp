// The consumer-side integration test the quick start walks through: implement
// the generated handler, then drive the generated server with the generated
// client over the loopback transport and a real socket.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "acme/todo/client.h"
#include "acme/todo/server.h"
#include "smithy/client/config.h"
#include "smithy/http/loopback.h"
#include "smithy/http/socket_transport.h"

namespace {

using acme::todo::AddTaskInput;
using acme::todo::AddTaskOutput;
using acme::todo::GetTaskInput;
using acme::todo::GetTaskOutput;
using acme::todo::NoSuchTask;
using acme::todo::TodoClient;
using acme::todo::TodoHandler;
using acme::todo::TodoServer;

class InMemoryHandler final : public TodoHandler {
 public:
  smithy::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input) override {
    const std::string id = "task-" + std::to_string(next_id_++);
    titles_[id] = input.title;
    return AddTaskOutput{.taskId = id, .title = input.title};
  }

  smithy::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input) override {
    const auto it = titles_.find(input.taskId);
    if (it == titles_.end()) {
      smithy::Error error = smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
      error.set_detail(NoSuchTask{.message = "no task: " + input.taskId});
      return error;
    }
    return GetTaskOutput{.taskId = input.taskId, .title = it->second, .done = false};
  }

 private:
  int next_id_ = 1;
  std::map<std::string, std::string> titles_;
};

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

}  // namespace
