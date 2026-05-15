#include "flock_session.hpp"

#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

FlockSession::FlockSession(string session_id_p) : session_id(std::move(session_id_p)) {
}

FlockSession::FlockSession(string session_id_p, string owner_principal_id_p)
    : session_id(std::move(session_id_p)), owner_principal_id(std::move(owner_principal_id_p)) {
}

FlockSession::~FlockSession() {
}

SessionManager::SessionManager(weak_ptr<DatabaseInstance> db_p) : db(std::move(db_p)) {
}

SessionManager::~SessionManager() {
}

string SessionManager::CreateNewConnection(const string &session_id) {
	std::lock_guard<std::mutex> lock(active_mutex);

	if (active.find(session_id) != active.end()) {
		throw InternalException("FlockSession id collision for '%s'", session_id);
	}

	auto db_locked = db.lock();
	if (!db_locked) {
		throw InternalException("Database was closed");
	}

	auto session = make_shared_ptr<FlockSession>(session_id);
	session->duckdb_connection = make_uniq<Connection>(*db_locked);
	session->duckdb_connection->context->config.enable_progress_bar = false;
	active[session_id] = std::move(session);
	return session_id;
}

shared_ptr<FlockSession> SessionManager::GetConnection(const string &session_id) {
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
// flock_crypto::PrincipalIdHex's output. Permits empty as a sentinel
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
	if (!config.TryGetCurrentSetting("flock_max_sessions", setting_val) || setting_val.IsNull()) {
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
	{
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
		throw InvalidInputException("flock session limit reached (flock_max_sessions=%llu)",
		                            static_cast<unsigned long long>(max_sessions));
	}
	if (active.find(session_id) != active.end()) {
		throw InternalException("FlockSession id collision for '%s'", session_id);
	}

	auto db_locked = db.lock();
	if (!db_locked) {
		throw InternalException("Database was closed");
	}

	auto session = make_shared_ptr<FlockSession>(session_id, owner_principal_id);
	session->duckdb_connection = make_uniq<Connection>(*db_locked);
	session->duckdb_connection->context->config.enable_progress_bar = false;
	active[session_id] = std::move(session);
	return session_id;
}

shared_ptr<FlockSession> SessionManager::LookupOwnedSession(const string &session_id, const string &principal_id) {
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
