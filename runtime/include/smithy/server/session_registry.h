#ifndef SMITHY_SERVER_SESSION_REGISTRY_H_
#define SMITHY_SERVER_SESSION_REGISTRY_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "smithy/eventstream/event_stream.h"

namespace smithy::server {

// The multi-client fan-out helper (issue #112): a thread-safe map of owning
// session handles (EventStream::Share) with a bounded outbound queue per
// session, so "N connected players, server pushes state to all of them"
// stops being ~200 hand-rolled lines of borrowed references and blocking
// send loops per consumer. Tx is the event union the sessions transmit; Id
// is the application's session key (player id, connection id).
//
//   smithy::server::SessionRegistry<RoomEvents> registry;
//   registry.Add(player_id, stream.Share());              // handler entry
//   registry.SendTo(player_id, event);                    // queued, non-blocking
//   registry.Broadcast(ids, [&](const Id& id) { return RedactFor(id); });
//   registry.CloseAll();                                  // the drain recipe, once
//   ...
//   registry.Remove(player_id);                           // handler exit
//
// The queue is the load-bearing decision: SendTo/Broadcast enqueue and
// return, and a per-session writer thread (started by Add) delivers in FIFO
// order — so a broadcast never stalls the room behind the slowest client's
// TCP window. When a session's queue is full the event is dropped and the
// slow-consumer policy runs: by default the session is closed (the Go-hub
// answer — its handler observes the close, returns, and removes itself);
// Options::on_slow_consumer replaces that default, keeping policy with the
// application. Per-recipient construction (the Broadcast(ids, make)
// overloads) exists because broadcast-identical-bytes is the wrong
// primitive for per-viewer state: make runs once per recipient, outside all
// registry locks.
//
// Lifecycle: Add on handler entry, Remove on handler exit — but unlike the
// borrowed-reference registry this replaces, a late Remove is a soft bug
// (stale handles fail with Error::Transport; nothing dangles). Remove never
// closes the session and discards its undelivered events; a writer mid-Send
// finishes or fails that event first, then exits. CloseAll closes every
// registered session; each blocked handler wakes, returns, and Removes
// itself, which is why Drain (CloseAll, then wait until the map empties) is
// the graceful-shutdown step to run before a transport Stop() — Stop()
// aborts live sessions rather than draining them (ADR-0015). The registry's
// destructor closes every remaining session and joins every writer, so a
// registry can never outlive-crash its threads.
//
// All methods are safe from any thread, including a handler's own (a
// handler may SendTo itself) and the slow-consumer callback (which runs
// with no registry locks held — it may call back into the registry, but
// keep it quick: it runs on whichever thread hit the full queue).
template <typename Tx, typename Id = std::string>
class SessionRegistry {
 public:
  using Handle = std::shared_ptr<eventstream::EventStreamHandle<Tx>>;

  struct Options {
    // Bound of each session's outbound queue — the burst a client may fall
    // behind by before the slow-consumer policy runs. Values below 1 mean 1.
    std::size_t queue_capacity = 64;
    // Runs instead of the default close-on-full when a session's queue is
    // full (the event is dropped either way). No registry locks are held.
    std::function<void(const Id&)> on_slow_consumer;
  };

  SessionRegistry() : SessionRegistry(Options{}) {}
  explicit SessionRegistry(Options options) : options_(Normalize(std::move(options))) {}

  // Closes every remaining session and joins every writer thread. Sessions
  // still registered here were leaked by their handlers (Remove-on-exit);
  // closing them is what lets the join terminate.
  ~SessionRegistry() {
    std::vector<std::shared_ptr<Entry>> all;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (auto& [id, entry] : sessions_) all.push_back(std::move(entry));
      sessions_.clear();
      all.insert(all.end(), std::make_move_iterator(retired_.begin()),
                 std::make_move_iterator(retired_.end()));
      retired_.clear();
    }
    for (const auto& entry : all) {
      entry->handle->Close();  // unblocks a writer mid-Send
      RequestStop(*entry);
    }
    for (const auto& entry : all) entry->writer.join();
  }

  SessionRegistry(const SessionRegistry&) = delete;
  SessionRegistry& operator=(const SessionRegistry&) = delete;

  // Registers a session under id and starts its writer. False (and nothing
  // changes — in particular the handle is not closed) when id is already
  // registered or handle is null; a reconnect under the same id wants
  // Remove first, which is the application's call to make.
  bool Add(Id id, Handle handle) {
    if (handle == nullptr) return false;
    auto entry = std::make_shared<Entry>(std::move(handle));
    const std::lock_guard<std::mutex> lock(mutex_);
    ReapLocked();
    if (!sessions_.emplace(std::move(id), entry).second) return false;
    // Under the lock, so nothing can observe (or retire) an entry whose
    // writer member is not yet set.
    entry->writer = std::thread([entry] { WriterLoop(*entry); });
    return true;
  }

  // Deregisters id: its undelivered events are discarded and its writer
  // exits (after finishing any event mid-Send). Remove never closes the
  // session — the handler that owns the stream usually calls this on its
  // way out, when the session is already over; closing a live session is
  // CloseAll's or the handle's job. False when id is not registered.
  bool Remove(const Id& id) {
    std::shared_ptr<Entry> entry;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const auto it = sessions_.find(id);
      if (it == sessions_.end()) return false;
      entry = std::move(it->second);
      sessions_.erase(it);
      retired_.push_back(entry);
      ReapLocked();
    }
    RequestStop(*entry);
    drained_.notify_all();
    return true;
  }

  // Queues one event for id; the writer delivers it in FIFO order. True
  // when queued. False — the event is dropped — for an unknown id, a
  // session whose delivery already failed, or a full queue (after running
  // the slow-consumer policy).
  bool SendTo(const Id& id, Tx event) {
    std::shared_ptr<Entry> entry;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const auto it = sessions_.find(id);
      if (it == sessions_.end()) return false;
      entry = it->second;
    }
    return Enqueue(id, *entry, std::move(event));
  }

  // Queues make(id)'s event for each registered id in ids (unknown ids are
  // skipped; make runs only for registered ones, outside all registry
  // locks). Returns how many were queued.
  std::size_t Broadcast(const std::vector<Id>& ids, const std::function<Tx(const Id&)>& make) {
    // Borrow the caller's ids rather than copying them; they outlive the call.
    std::vector<std::pair<const Id*, std::shared_ptr<Entry>>> targets;
    targets.reserve(ids.size());
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (const Id& id : ids) {
        if (const auto it = sessions_.find(id); it != sessions_.end()) {
          targets.emplace_back(&id, it->second);
        }
      }
    }
    std::size_t queued = 0;
    for (auto& [id, entry] : targets) {
      if (Enqueue(*id, *entry, make(*id))) ++queued;
    }
    return queued;
  }

  // The identical-bytes convenience of the above.
  std::size_t Broadcast(const std::vector<Id>& ids, const Tx& event) {
    return Broadcast(ids, [&event](const Id&) { return event; });
  }

  // Broadcast to every currently registered session (one registry pass, no
  // intermediate Ids() snapshot).
  std::size_t Broadcast(const std::function<Tx(const Id&)>& make) {
    std::vector<std::pair<Id, std::shared_ptr<Entry>>> targets;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      targets.reserve(sessions_.size());
      for (const auto& [id, entry] : sessions_) targets.emplace_back(id, entry);
    }
    std::size_t queued = 0;
    for (auto& [id, entry] : targets) {
      if (Enqueue(id, *entry, make(id))) ++queued;
    }
    return queued;
  }
  std::size_t Broadcast(const Tx& event) {
    return Broadcast([&event](const Id&) { return event; });
  }

  // Closes every registered session (idempotent, non-blocking): each
  // blocked handler wakes and returns, unregistering itself on the way out.
  void CloseAll() {
    std::vector<std::shared_ptr<Entry>> snapshot;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      snapshot.reserve(sessions_.size());
      for (const auto& [id, entry] : sessions_) snapshot.push_back(entry);
    }
    for (const auto& entry : snapshot) entry->handle->Close();
  }

  // The graceful-shutdown step (issue #112 proposal 3): CloseAll, then wait
  // for the handlers to Remove themselves. True once the registry is empty;
  // false on timeout (some handler is stuck or forgot Remove) — after
  // which the transport's Stop() aborts whatever is left, per ADR-0015.
  bool Drain(std::chrono::milliseconds timeout) {
    CloseAll();
    std::unique_lock<std::mutex> lock(mutex_);
    return drained_.wait_for(lock, timeout, [this] { return sessions_.empty(); });
  }

  // A snapshot of the registered ids — the "everyone currently here" input
  // to the Broadcast(ids, ...) overloads.
  std::vector<Id> Ids() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Id> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, entry] : sessions_) ids.push_back(id);
    return ids;
  }

  std::size_t size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
  }

 private:
  struct Entry {
    explicit Entry(Handle h) : handle(std::move(h)) {}
    const Handle handle;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Tx> queue;
    bool stopping = false;  // the writer must exit: Remove/teardown asked,
                            // or its own delivery failed (queue discarded)
    bool done = false;      // the writer exited; join is now instant
    std::thread writer;     // started by Add, joined by Reap/destructor
  };

  static Options Normalize(Options options) {
    if (options.queue_capacity < 1) options.queue_capacity = 1;
    return options;
  }

  // The per-session delivery loop: FIFO off the queue, one blocking Send at
  // a time. A failed Send is terminal for delivery (the session is closed
  // or gone) — remaining events are discarded and the writer exits; the
  // handler side observes the same failure through its own stream.
  static void WriterLoop(Entry& entry) {
    for (;;) {
      Tx event;
      {
        std::unique_lock<std::mutex> lock(entry.mutex);
        entry.wake.wait(lock, [&entry] { return entry.stopping || !entry.queue.empty(); });
        if (entry.stopping) break;
        event = std::move(entry.queue.front());
        entry.queue.pop_front();
      }
      if (!entry.handle->Send(event).ok()) {
        const std::lock_guard<std::mutex> lock(entry.mutex);
        entry.stopping = true;
        entry.queue.clear();
        break;
      }
    }
    const std::lock_guard<std::mutex> lock(entry.mutex);
    entry.done = true;
  }

  // Ask the writer to exit, discarding undelivered events. (The writer's
  // own failure path sets the same state inline, under its held lock.)
  static void RequestStop(Entry& entry) {
    {
      const std::lock_guard<std::mutex> lock(entry.mutex);
      entry.stopping = true;
      entry.queue.clear();
    }
    entry.wake.notify_all();
  }

  bool Enqueue(const Id& id, Entry& entry, Tx event) {
    bool queued = false;
    {
      const std::lock_guard<std::mutex> lock(entry.mutex);
      if (entry.stopping) return false;
      if (entry.queue.size() < options_.queue_capacity) {
        entry.queue.push_back(std::move(event));
        queued = true;
      }
    }
    if (queued) {
      entry.wake.notify_one();  // outside the lock: the writer wakes runnable
      return true;
    }
    // Full queue: drop the event, let policy decide the session's fate —
    // with no locks held, so the callback may call back into the registry.
    if (options_.on_slow_consumer) {
      options_.on_slow_consumer(id);
    } else {
      entry.handle->Close();
    }
    return false;
  }

  // Joins retired writers that have finished (done == true, so the join is
  // instant); called under mutex_ from Add/Remove so a churny hub never
  // accumulates unjoined threads. Writers still delivering stay retired
  // until a later reap or the destructor.
  void ReapLocked() {
    auto it = retired_.begin();
    while (it != retired_.end()) {
      bool done = false;
      {
        const std::lock_guard<std::mutex> lock((*it)->mutex);
        done = (*it)->done;
      }
      if (done) {
        (*it)->writer.join();
        it = retired_.erase(it);
      } else {
        ++it;
      }
    }
  }

  const Options options_;
  mutable std::mutex mutex_;
  std::condition_variable drained_;
  std::map<Id, std::shared_ptr<Entry>> sessions_;
  std::vector<std::shared_ptr<Entry>> retired_;
};

}  // namespace smithy::server

#endif  // SMITHY_SERVER_SESSION_REGISTRY_H_
