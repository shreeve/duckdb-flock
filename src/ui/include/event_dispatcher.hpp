#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

namespace duckdb_httplib_openssl {
class DataSink;
}

namespace duckdb {

namespace ui {

class EventDispatcher {
public:
  void SendConnectedEvent(const std::string &token);
  void SendCatalogChangedEvent();

  bool WaitEvent(duckdb_httplib_openssl::DataSink *sink);
  void Close();

private:
  void SendEvent(const std::string &message);
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic_int next_id{0};
  std::atomic_int current_id{-1};
  std::atomic_int wait_count{0};
  std::string message;
  std::atomic_bool closed{false};
};
} // namespace ui
} // namespace duckdb
