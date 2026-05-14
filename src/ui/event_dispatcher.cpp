#include "event_dispatcher.hpp"

#include <duckdb.hpp>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

namespace httplib = duckdb_httplib_openssl;

// Chosen to be no more than half of the lesser of the two limits:
//  - The default httplib thread pool size = 8
//  - The browser limit on the number of server-sent event connections = 6
#define MAX_EVENT_WAIT_COUNT 3

namespace duckdb {
namespace ui {
// An empty Server-Sent Events message. See
// https://html.spec.whatwg.org/multipage/server-sent-events.html#authoring-notes
constexpr const char *EMPTY_SSE_MESSAGE = ":\r\r";
constexpr idx_t EMPTY_SSE_MESSAGE_LENGTH = 3;

bool EventDispatcher::WaitEvent(httplib::DataSink *sink) {
  std::unique_lock<std::mutex> lock(mutex);
  // Don't allow too many simultaneous waits, because each consumes a thread in
  // the httplib thread pool, and also browsers limit the number of server-sent
  // event connections.
  if (closed || wait_count >= MAX_EVENT_WAIT_COUNT) {
    return false;
  }
  int target_id = next_id;
  wait_count++;
  cv.wait_for(lock, std::chrono::seconds(5));
  wait_count--;
  if (closed) {
    return false;
  }
  if (current_id == target_id) {
    sink->write(message.data(), message.size());
  } else {
    // Our wait timer expired. Write an empty, no-op message.
    // This enables detecting when the client is gone.
    sink->write(EMPTY_SSE_MESSAGE, EMPTY_SSE_MESSAGE_LENGTH);
  }
  return true;
}

void EventDispatcher::SendEvent(const std::string &_message) {
  std::lock_guard<std::mutex> guard(mutex);
  if (closed) {
    return;
  }

  current_id = next_id++;
  message = _message;
  cv.notify_all();
}

void EventDispatcher::SendConnectedEvent(const std::string &token) {
  SendEvent(StringUtil::Format("event: ConnectedEvent\ndata: %s\n\n", token));
}

void EventDispatcher::SendCatalogChangedEvent() {
  SendEvent("event: CatalogChangeEvent\ndata:\n\n");
}

void EventDispatcher::Close() {
  std::lock_guard<std::mutex> guard(mutex);
  if (closed) {
    return;
  }

  current_id = next_id++;
  closed = true;
  cv.notify_all();
}
} // namespace ui
} // namespace duckdb
