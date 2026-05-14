#pragma once

// PR-3 transitional fork — see docs/upstream-ui-patches.md.
//
// Decoupled from upstream's HttpServer: Watcher now takes its
// dependencies (database weak_ptr, event dispatcher, polling interval)
// directly via the constructor instead of pulling them off an
// HttpServer reference. Lets it be owned by UiHandlers (which doesn't
// inherit upstream's HttpServer interface) without forcing UiHandlers
// to expose upstream's API.

#include <atomic>
#include <condition_variable>
#include <duckdb.hpp>
#include <mutex>
#include <thread>

namespace duckdb {
namespace ui {

struct CatalogState {
	std::map<idx_t, optional_idx> db_to_catalog_version;
};

class EventDispatcher;

class Watcher {
public:
	Watcher(weak_ptr<DatabaseInstance> db, EventDispatcher &dispatcher, uint32_t polling_interval_ms);

	void Start();
	void Stop();

private:
	void Watch();

	unique_ptr<std::thread> thread;
	std::mutex mutex;
	std::condition_variable cv;
	std::atomic<bool> should_run;

	weak_ptr<DatabaseInstance> ddb_instance;
	EventDispatcher &dispatcher;
	uint32_t polling_interval_ms;

	DatabaseInstance *watched_database;
};

} // namespace ui
} // namespace duckdb
