// PR-3 transitional fork — see docs/upstream-ui-patches.md.
//
// Decoupled from upstream's HttpServer dependency. Now takes
// (weak_ptr<DatabaseInstance>, EventDispatcher&, polling_interval_ms)
// directly. Logic is otherwise upstream's verbatim.

#include "watcher.hpp"

#include <duckdb/main/attached_database.hpp>

#include "event_dispatcher.hpp"
#include "settings.hpp"
#include "utils/helpers.hpp"
#include "utils/md_helpers.hpp"

namespace duckdb {
namespace ui {

Watcher::Watcher(weak_ptr<DatabaseInstance> db, EventDispatcher &dispatcher_p, uint32_t polling_interval_ms_p)
    : should_run(false), ddb_instance(std::move(db)), dispatcher(dispatcher_p),
      polling_interval_ms(polling_interval_ms_p), watched_database(nullptr) {
}

bool WasCatalogUpdated(DatabaseInstance &db, Connection &connection, CatalogState &last_state) {
	bool has_change = false;
	auto &context = *connection.context;
	connection.BeginTransaction();

	const auto &databases = db.GetDatabaseManager().GetDatabases(context);
	std::set<idx_t> db_oids;

	// Check currently attached databases
	for (const auto &db_ref : databases) {
#if DUCKDB_VERSION_AT_MOST(1, 3, 2)
		auto &db_instance = db_ref.get();
#else
		auto &db_instance = *db_ref;
#endif
		if (db_instance.IsTemporary()) {
			continue; // ignore temp databases
		}

		db_oids.insert(db_instance.oid);
		auto &catalog = db_instance.GetCatalog();
		auto current_version = catalog.GetCatalogVersion(context);
		auto last_version_it = last_state.db_to_catalog_version.find(db_instance.oid);
		if (last_version_it == last_state.db_to_catalog_version.end() // first time
		    || !(last_version_it->second == current_version)) {       // updated
			has_change = true;
			last_state.db_to_catalog_version[db_instance.oid] = current_version;
		}
	}

	// Now check if any databases have been detached
	for (auto it = last_state.db_to_catalog_version.begin(); it != last_state.db_to_catalog_version.end();) {
		if (db_oids.find(it->first) == db_oids.end()) {
			has_change = true;
			it = last_state.db_to_catalog_version.erase(it);
		} else {
			++it;
		}
	}

	connection.Rollback();
	return has_change;
}

void Watcher::Watch() {
	CatalogState last_state;
	bool is_md_connected = false;
	while (should_run) {
		auto db = ddb_instance.lock();
		if (!db) {
			break; // DB went away, nothing to watch
		}

		if (watched_database == nullptr) {
			watched_database = db.get();
		} else if (watched_database != db.get()) {
			break; // DB changed, stop watching, will be restarted
		}

		duckdb::Connection con {*db};
		if (polling_interval_ms == 0) {
			return; // Disable watcher
		}

		try {
			if (WasCatalogUpdated(*db, con, last_state)) {
				dispatcher.SendCatalogChangedEvent();
			}

			if (!is_md_connected && IsMDConnected(con)) {
				is_md_connected = true;
				dispatcher.SendConnectedEvent(GetMDToken(con));
			}
		} catch (std::exception &ex) {
			// Do not crash with uncaught exception, but quit.
			std::cerr << "Error in watcher: " << ex.what() << std::endl;
			std::cerr << "Will now terminate." << std::endl;
			return;
		}

		{
			std::unique_lock<std::mutex> lock(mutex);
			cv.wait_for(lock, std::chrono::milliseconds(polling_interval_ms));
		}
	}
}

void Watcher::Start() {
	{
		std::lock_guard<std::mutex> guard(mutex);
		should_run = true;
	}

	if (!thread) {
		thread = make_uniq<std::thread>(&Watcher::Watch, this);
	}
}

void Watcher::Stop() {
	if (!thread) {
		return;
	}

	{
		std::lock_guard<std::mutex> guard(mutex);
		should_run = false;
	}
	cv.notify_all();
	thread->join();
	thread.reset();
}

} // namespace ui
} // namespace duckdb
