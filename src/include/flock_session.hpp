#pragma once

// SessionManager and FlockSession — the per-process DB session pool.
//
// Lifted from upstream QuackServer's connection-pool members
// (active_connections + session_id_rng) so that PR-3+ handler subsystems
// (UiHandlers, SqlHandlers) can share the same pool with QuackHandlers.
//
// PR-2 keeps the existing semantics (no idle TTL, no per-principal
// scoping) — those land in PR-3/PR-4 with the auth-cookie/principal
// model. The struct shape of FlockSession is identical to upstream
// QuackConnection; only the name changes to align with SPEC §6
// vocabulary.

#include <memory>
#include <mutex>
#include <unordered_map>

#include "duckdb/common/common.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

class DatabaseInstance;
class Connection;
class QueryResult;
class EncryptionState;

// Per-session state. Identical fields to upstream QuackConnection.
//
// Owned by SessionManager via shared_ptr; handlers borrow shared_ptr
// for the duration of a request to keep the session alive even if a
// concurrent disconnect erases the map entry mid-request (a real race
// the previous optional_ptr design would have hit).
struct FlockSession {
	explicit FlockSession(string session_id_p);
	~FlockSession();

	mutex lock;
	unique_ptr<Connection> duckdb_connection;
	unique_ptr<QueryResult> duckdb_query_result;
	//! Monotonic counter assigned per FETCH batch — enables order-preserving
	//! parallel scans on the client side.
	idx_t next_batch_index = 1;
	//! Current result UUID, set per PREPARE.
	hugeint_t result_uuid;
	string session_id;
};

// Per-process session pool. Holds the active map + the lazy-init CSPRNG
// used for session-id generation. Shared by every handler subsystem
// that needs to look up a session by id; passed by reference into
// handler ctors so the same pool is visible across /quack, /sql, /ddb
// etc. once those handlers exist.
class SessionManager {
public:
	explicit SessionManager(weak_ptr<DatabaseInstance> db);
	~SessionManager();

	SessionManager(const SessionManager &) = delete;
	SessionManager &operator=(const SessionManager &) = delete;

	// Create a fresh session keyed by the given id. Throws InternalException
	// if the id collides with an existing session (callers should pass a
	// freshly-generated id from GenerateSessionId()). Returns the session id
	// back, mirroring upstream QuackServer::CreateNewConnection so refactored
	// callers stay shape-compatible.
	string CreateNewConnection(const string &session_id);

	// Look up a session by id; returns nullptr if not found. The returned
	// shared_ptr keeps the session alive for the caller's lifetime even if
	// a concurrent DisconnectConnection erases the map entry.
	shared_ptr<FlockSession> GetConnection(const string &session_id);

	// Destroy a session by id. Returns false if the id was not registered.
	// Idempotent for the "not-found" case.
	bool DisconnectConnection(const string &session_id);

	// Generate a fresh CSPRNG-backed 16-byte (128-bit) session id, hex-encoded
	// (32 chars). Lazy-initializes the RNG via db.GetEncryptionUtil() on first
	// call. Thread-safe; the rng_mutex guards lazy-init only — actual byte
	// generation is single-call into DuckDB's encryption util.
	string GenerateSessionId();

	// Number of currently-active sessions. Snapshot value; may change between
	// the call and any subsequent GetConnection.
	idx_t ActiveCount();

private:
	weak_ptr<DatabaseInstance> db;

	std::mutex active_mutex;
	unordered_map<string, shared_ptr<FlockSession>> active;

	std::mutex rng_mutex;
	shared_ptr<EncryptionState> rng;
};

} // namespace duckdb
