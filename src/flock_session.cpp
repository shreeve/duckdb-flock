#include "flock_session.hpp"

#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

FlockSession::FlockSession(string session_id_p) : session_id(std::move(session_id_p)) {
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

} // namespace duckdb
