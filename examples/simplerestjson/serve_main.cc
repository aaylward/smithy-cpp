// The serving lifecycle, end to end (issue #49): SIGTERM/SIGINT → drained
// shutdown. Run it, add and fetch a book, stop it:
//
//   bazel run //examples/simplerestjson:bookstore_server
//   curl -X POST localhost:8080/books -d '{"isbn":"0-306-40615-2","title":"Petriflora"}'
//   curl localhost:8080/books/0-306-40615-2
//   kill -TERM <pid>   # or Ctrl-C
//
// The pattern: block the shutdown signals before Start() so the transport's
// worker threads inherit the mask and the signals are delivered only to the
// sigwait() below; serve until one arrives; then Stop(), which drains — no
// new connections or keep-alive reads, and in-flight requests get
// Options.drain_timeout_seconds to finish. Under Kubernetes, size
// terminationGracePeriodSeconds above the drain timeout, and compose the
// /livez + /readyz probes from smithy/server/middleware.h in front of the
// handler (production-guide.md shows the chain).

#include <csignal>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "examples/simplerestjson/generated/include/example/bookstore/server.h"
#include "examples/simplerestjson/generated/include/example/bookstore/types.h"
#include "smithy/core/outcome.h"
#include "smithy/http/beast_transport.h"

namespace {

using example::bookstore::AddBookInput;
using example::bookstore::AddBookOutput;
using example::bookstore::BookNotFound;
using example::bookstore::BookstoreServer;
using example::bookstore::GetBookInput;
using example::bookstore::GetBookOutput;

// Handlers must be thread-safe: the transport dispatches on a thread pool.
class InMemoryBookstore final : public example::bookstore::BookstoreHandler {
 public:
  smithy::Outcome<AddBookOutput> AddBook(const AddBookInput& input) override {
    const std::lock_guard<std::mutex> lock(mu_);
    titles_[input.isbn] = input.title;
    return AddBookOutput{.status = 201, .isbn = input.isbn};
  }

  smithy::Outcome<GetBookOutput> GetBook(const GetBookInput& input) override {
    const std::lock_guard<std::mutex> lock(mu_);
    const auto it = titles_.find(input.isbn);
    if (it == titles_.end()) {
      smithy::Error error = smithy::Error::Modeled("BookNotFound", "no book: " + input.isbn);
      error.set_detail(BookNotFound{.message = "no book: " + input.isbn, .isbn = input.isbn});
      return error;  // the server turns this into the modeled 404
    }
    return GetBookOutput{.isbn = input.isbn, .title = it->second};
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::string> titles_;
};

}  // namespace

int main() {
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  // Before Start(): threads the transport creates inherit this mask, so the
  // shutdown signals reach only the sigwait() below.
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

  BookstoreServer server(std::make_shared<InMemoryBookstore>());
  smithy::http::BeastServerTransport transport({
      .address = "0.0.0.0",
      .port = 8080,
      .drain_timeout_seconds = 10,
  });
  smithy::Outcome<smithy::Unit> started = transport.Start(server.Handler());
  if (!started.ok()) {
    std::fprintf(stderr, "bookstore: start failed: %s\n", started.error().message().c_str());
    return 1;
  }
  std::fprintf(stderr, "bookstore: serving on :%d (SIGTERM or Ctrl-C drains and exits)\n",
               transport.port());

  int signal_number = 0;
  sigwait(&shutdown_signals, &signal_number);
  std::fprintf(stderr, "bookstore: signal %d, draining\n", signal_number);
  transport.Stop();  // in-flight requests get drain_timeout_seconds to finish
  return 0;
}
