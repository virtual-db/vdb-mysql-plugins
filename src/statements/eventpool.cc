/* eventpool.cc: the event connections (doc 08 §5, product doc 04 §2.3).
   Four connections serve synchronous calls (Rewrite, Ping, DdlCaptured,
   QueryReceived); a fifth carries asynchronous Events from a bounded queue,
   which drops and counts when full so a statement never waits on audit. */
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "storage/vdb/statements/statements.h"

unsigned long long vdb_st_events_dropped, vdb_st_rewrites;

namespace {
constexpr int kSync = 4;
constexpr size_t kQueueCap = 10000;

struct Slot {
  std::mutex mu;
  Vdb_conn conn;
};
Slot slots[kSync];

std::mutex qmu;
std::condition_variable qcv;
std::deque<std::string> queue;
bool stopping = false;
std::thread worker;

void run_worker() {
  Vdb_conn conn;
  std::string err;
  for (;;) {
    std::string body;
    {
      std::unique_lock<std::mutex> l(qmu);
      qcv.wait(l, [] { return stopping || !queue.empty(); });
      if (stopping && queue.empty()) return;
      body = std::move(queue.front());
      queue.pop_front();
    }
    if (!conn.is_open() && !conn.connect(vdb::CONN_EVENT, 0, &err)) {
      __atomic_add_fetch(&vdb_st_events_dropped, 1, __ATOMIC_RELAXED);
      continue;
    }
    vdb::Event e;
    e.ParseFromString(body);
    if (!conn.send(vdb_proto::OP_EVENT, e)) __atomic_add_fetch(&vdb_st_events_dropped, 1, __ATOMIC_RELAXED);
  }
}
}  // namespace

int vdb_event_call(uint8_t op, const google::protobuf::MessageLite &req, google::protobuf::MessageLite *reply,
                   vdb::Error *err, bool *query_intercept) {
  static std::atomic<unsigned> next{0};
  Slot &s = slots[next.fetch_add(1) % kSync];
  std::lock_guard<std::mutex> l(s.mu);
  std::string why;
  if (!s.conn.is_open() && !s.conn.connect(vdb::CONN_EVENT, 0, &why)) {
    if (err) err->set_message(why);
    return -1;
  }
  if (query_intercept) *query_intercept = s.conn.query_intercept();
  if (op == 0) return 0;  // only the connection's Hello flags were wanted
  return s.conn.call(op, req, reply, err);
}

void vdb_event_send(const vdb::Event &e) {
  std::lock_guard<std::mutex> l(qmu);
  if (queue.size() >= kQueueCap || stopping) {
    __atomic_add_fetch(&vdb_st_events_dropped, 1, __ATOMIC_RELAXED);
    return;
  }
  queue.push_back(e.SerializeAsString());
  qcv.notify_one();
}

void vdb_events_start() {
  std::lock_guard<std::mutex> l(qmu);
  stopping = false;
  worker = std::thread(run_worker);
}

void vdb_events_stop() {
  {
    std::lock_guard<std::mutex> l(qmu);
    stopping = true;
  }
  qcv.notify_all();
  if (worker.joinable()) worker.join();
  for (Slot &s : slots) {
    std::lock_guard<std::mutex> l(s.mu);
    s.conn.close();
  }
}
