#include "harbor_session.hpp"

#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

HarborSession::HarborSession(string session_id_p)
    : session_id(std::move(session_id_p)), created_at(std::chrono::steady_clock::now()) {
}

HarborSession::HarborSession(string session_id_p, string owner_principal_id_p)
    : session_id(std::move(session_id_p)), owner_principal_id(std::move(owner_principal_id_p)),
      created_at(std::chrono::steady_clock::now()) {
}

HarborSession::~HarborSession() {
}

SessionManager::SessionManager(weak_ptr<DatabaseInstance> db_p) : db(std::move(db_p)) {
}

SessionManager::~SessionManager() {
	// PR-7b — clean shutdown of the timeout sweeper. Signal under
	// the sweeper mutex, notify the cv to wake the sleeping wait_for,
	// then join. If StartTimeoutSweeper() was never called the
	// thread is empty and joinable() is false.
	{
		std::lock_guard<std::mutex> lock(sweeper_mutex);
		shutdown_requested = true;
	}
	sweeper_cv.notify_all();
	if (sweeper_thread.joinable()) {
		sweeper_thread.join();
	}
}

string SessionManager::CreateNewConnection(const string &session_id) {
	std::lock_guard<std::mutex> lock(active_mutex);

	if (active.find(session_id) != active.end()) {
		throw InternalException("HarborSession id collision for '%s'", session_id);
	}

	auto db_locked = db.lock();
	if (!db_locked) {
		throw InternalException("Database was closed");
	}

	auto session = make_shared_ptr<HarborSession>(session_id);
	session->duckdb_connection = make_uniq<Connection>(*db_locked);
	session->duckdb_connection->context->config.enable_progress_bar = false;
	active[session_id] = std::move(session);
	return session_id;
}

shared_ptr<HarborSession> SessionManager::GetConnection(const string &session_id) {
	std::lock_guard<std::mutex> lock(active_mutex);
	auto it = active.find(session_id);
	if (it == active.end()) {
		return nullptr;
	}
	// Returning a copy of the shared_ptr (not a reference) keeps the session
	// alive for the caller even if a concurrent DisconnectConnection erases
	// the map entry between this lookup and the caller's last use.
	return it->second;
}

bool SessionManager::DisconnectConnection(const string &session_id) {
	std::lock_guard<std::mutex> lock(active_mutex);
	auto it = active.find(session_id);
	if (it == active.end()) {
		return false;
	}
	active.erase(it);
	return true;
}

namespace {

constexpr idx_t kSessionIdBytes = 16; // 128 bits, hex-encoded → 32 chars
constexpr idx_t kDefaultMaxSessions = 1024; // SPEC §6 default

// True iff `s` is a 64-char lowercase hex string. Matches the shape of
// harbor_crypto::PrincipalIdHex's output. Permits empty as a sentinel
// for "anonymous" / legacy sessions (caller decides whether to allow).
bool IsValidPrincipalHex(const string &s) {
	if (s.size() != 64) {
		return false;
	}
	for (char c : s) {
		bool is_digit = (c >= '0' && c <= '9');
		bool is_lower_hex = (c >= 'a' && c <= 'f');
		if (!is_digit && !is_lower_hex) {
			return false;
		}
	}
	return true;
}

idx_t ReadMaxSessionsSetting(weak_ptr<DatabaseInstance> &db) {
	auto db_locked = db.lock();
	if (!db_locked) {
		return kDefaultMaxSessions;
	}
	Value setting_val;
	auto &config = DBConfig::GetConfig(*db_locked);
	if (!config.TryGetCurrentSetting("harbor_max_sessions", setting_val) || setting_val.IsNull()) {
		return kDefaultMaxSessions;
	}
	try {
		auto v = setting_val.GetValue<uint64_t>();
		return v == 0 ? kDefaultMaxSessions : v;
	} catch (...) {
		return kDefaultMaxSessions;
	}
}

string HexEncode(const data_t *bytes, idx_t n) {
	string result(n * 2, '\0');
	for (idx_t i = 0; i < n; i++) {
		result[2 * i] = Blob::HEX_TABLE[bytes[i] >> 4];
		result[2 * i + 1] = Blob::HEX_TABLE[bytes[i] & 0x0F];
	}
	return result;
}

} // namespace

string SessionManager::GenerateSessionId() {
	// PR-6 follow-up (round 19): hold rng_mutex across GenerateRandomData
	// too. Releasing the lock between init and generation was a TOCTOU
	// race under concurrent `/sql/sessions/new` — `EncryptionState`
	// instances aren't documented as concurrent-safe. Session creation
	// is not hot enough to optimize this; correctness wins.
	std::lock_guard<std::mutex> lock(rng_mutex);
	if (!rng) {
		auto db_locked = db.lock();
		if (!db_locked) {
			throw InternalException("Database was closed");
		}
		auto encryption_util = db_locked->GetEncryptionUtil(false);
		auto metadata = make_uniq<EncryptionStateMetadata>(
		    EncryptionTypes::GCM, kSessionIdBytes, EncryptionTypes::EncryptionVersion::NONE);
		rng = encryption_util->CreateEncryptionState(std::move(metadata));
	}

	data_t bytes[kSessionIdBytes];
	rng->GenerateRandomData(bytes, kSessionIdBytes);
	return HexEncode(bytes, kSessionIdBytes);
}

idx_t SessionManager::ActiveCount() {
	std::lock_guard<std::mutex> lock(active_mutex);
	return active.size();
}

string SessionManager::CreateOwnedSession(const string &session_id, const string &owner_principal_id) {
	if (!IsValidPrincipalHex(owner_principal_id)) {
		// Caller bug — /sql handlers always derive principal from
		// successful auth, which produces a 64-char hex.
		throw InvalidInputException("CreateOwnedSession: owner_principal_id must be 64-char lowercase hex");
	}
	auto max_sessions = ReadMaxSessionsSetting(db);

	std::lock_guard<std::mutex> lock(active_mutex);

	if (active.size() >= max_sessions) {
		// SPEC §6 limits table: "new session creation returns 429
		// SESSION_LIMIT". InvalidInputException is the closest
		// portable exception type; the SQL handler maps it to a 429
		// response with errorCode SESSION_LIMIT.
		throw InvalidInputException("harbor session limit reached (harbor_max_sessions=%llu)",
		                            static_cast<unsigned long long>(max_sessions));
	}
	if (active.find(session_id) != active.end()) {
		throw InternalException("HarborSession id collision for '%s'", session_id);
	}

	auto db_locked = db.lock();
	if (!db_locked) {
		throw InternalException("Database was closed");
	}

	auto session = make_shared_ptr<HarborSession>(session_id, owner_principal_id);
	session->duckdb_connection = make_uniq<Connection>(*db_locked);
	session->duckdb_connection->context->config.enable_progress_bar = false;
	active[session_id] = std::move(session);
	return session_id;
}

shared_ptr<HarborSession> SessionManager::LookupOwnedSession(const string &session_id, const string &principal_id) {
	std::lock_guard<std::mutex> lock(active_mutex);
	auto it = active.find(session_id);
	if (it == active.end()) {
		return nullptr;
	}
	// Constant-shape ownership check — same path for "wrong owner" and
	// "not found" so the caller cannot enumerate session ids by
	// observing 403-vs-404 timing/return differences (SPEC §6
	// "Unknown / foreign session ID returns 404").
	if (it->second->owner_principal_id != principal_id) {
		return nullptr;
	}
	return it->second;
}

bool SessionManager::DestroyOwnedSession(const string &session_id, const string &principal_id) {
	std::lock_guard<std::mutex> lock(active_mutex);
	auto it = active.find(session_id);
	if (it == active.end()) {
		return false;
	}
	if (it->second->owner_principal_id != principal_id) {
		return false;
	}
	active.erase(it);
	return true;
}

std::vector<SessionSnapshot> SessionManager::Snapshot(idx_t last_query_max_chars) {
	// Step 1: copy out shared_ptr<HarborSession>'s under the map lock,
	// then release. Per round-18 review: lock ordering is map -> session,
	// never both held simultaneously.
	std::vector<shared_ptr<HarborSession>> sessions_local;
	{
		std::lock_guard<std::mutex> lock(active_mutex);
		sessions_local.reserve(active.size());
		for (auto &kv : active) {
			sessions_local.push_back(kv.second);
		}
	}

	std::vector<SessionSnapshot> out;
	out.reserve(sessions_local.size());
	for (auto &session : sessions_local) {
		SessionSnapshot snap;
		snap.session_id = session->session_id;
		snap.owner_principal_id = session->owner_principal_id;
		snap.created_at = session->created_at;
		snap.query_in_flight = session->query_in_flight.load(std::memory_order_acquire);
		if (last_query_max_chars > 0) {
			std::lock_guard<duckdb::mutex> session_lock(session->lock);
			if (session->last_query.size() > last_query_max_chars) {
				snap.last_query = session->last_query.substr(0, last_query_max_chars);
				snap.last_query_truncated = true;
			} else {
				snap.last_query = session->last_query;
				snap.last_query_truncated = false;
			}
		}
		out.push_back(std::move(snap));
	}
	return out;
}

bool SessionManager::InterruptSession(const string &session_id, InterruptCause cause) {
	// Lookup under map lock; copy the shared_ptr so the session stays
	// alive for the Interrupt() call even if a concurrent disconnect
	// erases the map entry. Connection::Interrupt() is documented as
	// concurrency-safe (sets executor flag); we explicitly do NOT take
	// the per-session mutex (it's held by the in-flight Execute that
	// we're trying to interrupt — taking it would deadlock).
	shared_ptr<HarborSession> session;
	{
		std::lock_guard<std::mutex> lock(active_mutex);
		auto it = active.find(session_id);
		if (it == active.end()) {
			return false;
		}
		session = it->second;
	}
	if (session && session->duckdb_connection) {
		// PR-7b — record the cause BEFORE Interrupt() so the catch
		// path's classification observes it. Sweep-side TIMEOUT
		// interrupts go through TimeoutSweepOnce() instead, which
		// uses timed_out_generation as the more-precise per-query
		// signal. NONE callers (legacy code paths that didn't pass a
		// cause) default to USER_CANCEL — the only legacy callers
		// today are /sql/cancel and /interrupt, which IS USER_CANCEL.
		session->interrupt_cause.store(static_cast<uint8_t>(cause), std::memory_order_release);
		session->duckdb_connection->Interrupt();
	}
	return true;
}

// -- PR-7b timeout sweeper -------------------------------------------------

namespace {
constexpr int kTimeoutSweepTickMs = 250;
} // namespace

void SessionManager::StartTimeoutSweeper() {
	std::lock_guard<std::mutex> lock(sweeper_mutex);
	if (sweeper_started) {
		return;
	}
	sweeper_started = true;
	sweeper_thread = std::thread(&SessionManager::TimeoutSweepLoop, this);
}

void SessionManager::TimeoutSweepLoop() {
	std::unique_lock<std::mutex> lock(sweeper_mutex);
	while (!shutdown_requested) {
		// Wait up to kTimeoutSweepTickMs OR until shutdown is
		// signalled. The predicate form returns whether the
		// predicate is true at exit; we don't care about the return
		// value — both timeout-and-still-running and woken-by-shutdown
		// loop back to the loop head, which re-checks
		// `shutdown_requested`.
		sweeper_cv.wait_for(lock, std::chrono::milliseconds(kTimeoutSweepTickMs),
		                    [this] { return shutdown_requested; });
		if (shutdown_requested) {
			return;
		}
		// Drop the sweeper mutex during the actual sweep — we don't
		// need it while iterating sessions, and TimeoutSweepOnce
		// takes the active_mutex briefly. Holding sweeper_mutex
		// across that would block any future StartTimeoutSweeper()
		// pointlessly.
		lock.unlock();
		try {
			TimeoutSweepOnce();
		} catch (...) {
			// Swallow exceptions inside the sweeper — the only
			// supported failure modes here are session disappearing
			// mid-sweep (handled by shared_ptr lifetime) and
			// Connection::Interrupt() throwing (it doesn't, by
			// contract). Letting an exception escape would
			// std::terminate the process.
		}
		lock.lock();
	}
}

void SessionManager::TimeoutSweepOnce() {
	// Step 1 (per round-21 lock-ordering): copy shared_ptrs out of
	// the active map under map lock, then release.
	std::vector<shared_ptr<HarborSession>> sessions_local;
	{
		std::lock_guard<std::mutex> lock(active_mutex);
		sessions_local.reserve(active.size());
		for (auto &kv : active) {
			sessions_local.push_back(kv.second);
		}
	}

	auto now = std::chrono::steady_clock::now();
	auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

	for (auto &session : sessions_local) {
		// Step 2: read the atomics. No per-session lock needed.
		const bool in_flight = session->query_in_flight.load(std::memory_order_acquire);
		if (!in_flight) {
			continue;
		}
		const int64_t deadline_ms = session->query_deadline_ms.load(std::memory_order_acquire);
		if (deadline_ms == 0) {
			continue; // timeout disabled for this query
		}
		if (now_ms <= deadline_ms) {
			continue; // not overdue yet
		}

		// Step 3: read the generation, write timed_out_generation
		// BEFORE calling Interrupt(). The catch path / dtor of the
		// QueryExecutionGuard reads timed_out_generation == my_generation
		// to decide whether THIS query's cancellation was a timeout.
		const uint64_t gen = session->query_generation.load(std::memory_order_acquire);
		// If a concurrent QueryExecutionGuard dtor cleared the
		// deadline between our read and now, we still proceed —
		// recording timed_out_generation = gen is harmless if the
		// query already finished cleanly (the gen no longer matches
		// any live guard).
		session->timed_out_generation.store(gen, std::memory_order_release);

		// Connection::Interrupt() is concurrency-safe per its DuckDB
		// contract; we explicitly do NOT take session->lock here
		// (the in-flight Execute holds it; taking it would deadlock).
		if (session->duckdb_connection) {
			session->duckdb_connection->Interrupt();
		}
	}
}

idx_t SessionManager::DestroyAllOwnedBy(const string &principal_id) {
	if (principal_id.empty()) {
		// Refuse to destroy "everything that has no owner" — that
		// would catch the legacy QuackHandlers sessions, which the
		// caller (/auth/logout) has no business touching. The /sql
		// principal-aware path always passes a non-empty principal_id.
		return 0;
	}
	std::lock_guard<std::mutex> lock(active_mutex);
	idx_t destroyed = 0;
	for (auto it = active.begin(); it != active.end();) {
		if (it->second->owner_principal_id == principal_id) {
			it = active.erase(it);
			++destroyed;
		} else {
			++it;
		}
	}
	return destroyed;
}

} // namespace duckdb
