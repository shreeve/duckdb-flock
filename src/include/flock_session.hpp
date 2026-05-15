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
#include <unordered_map>

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {

class DatabaseInstance;
class Connection;
class QueryResult;
class EncryptionState;

// Per-session state. Originally lifted from upstream QuackConnection;
// PR-5 adds owner_principal_id (per SPEC §6 "A DB session is owned by
// exactly one principal").
//
// Owned by SessionManager via shared_ptr; handlers borrow shared_ptr
// for the duration of a request to keep the session alive even if a
// concurrent disconnect erases the map entry mid-request (a real race
// the previous optional_ptr design would have hit).
struct FlockSession {
	explicit FlockSession(string session_id_p);
	FlockSession(string session_id_p, string owner_principal_id_p);
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
	//! Principal that owns this session (per SPEC §6). Empty string means
	//! the session was created by the legacy CreateNewConnection() path
	//! (Quack /quack today; principal-aware migration of QuackHandlers
	//! is post-v0.1). New /sql sessions always have this set to a
	//! non-empty 64-char hex by CreateOwnedSession().
	string owner_principal_id;
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
	// callers stay shape-compatible. The created session has an EMPTY
	// owner_principal_id — used today by QuackHandlers, which has not been
	// migrated to principal ownership yet (post-v0.1).
	string CreateNewConnection(const string &session_id);

	// Look up a session by id; returns nullptr if not found. The returned
	// shared_ptr keeps the session alive for the caller's lifetime even if
	// a concurrent DisconnectConnection erases the map entry.
	//
	// NO ownership check. /sql callers should use LookupOwnedSession()
	// instead, which conflates "not found" and "wrong owner" into a
	// single nullptr return per SPEC §6 line 637-643 (anti-enumeration).
	shared_ptr<FlockSession> GetConnection(const string &session_id);

	// Destroy a session by id. Returns false if the id was not registered.
	// Idempotent for the "not-found" case. NO ownership check; used by
	// the legacy QuackHandlers DISCONNECT_MESSAGE path.
	bool DisconnectConnection(const string &session_id);

	// PR-5: principal-aware session lifecycle (per SPEC §6).
	//
	// Create a session owned by `owner_principal_id` (must be a 64-char
	// lowercase hex string per AuthManager::PrincipalIdHex). Throws
	// InvalidInputException if owner_principal_id is empty or malformed
	// (a precondition violation by the caller — /sql handlers always
	// derive principal from successful auth).
	//
	// Throws SESSION_LIMIT-mapped InvalidInputException if the active
	// session count would exceed `flock_max_sessions` (default 1024).
	// Note: the limit applies to the GLOBAL active count, not per-principal,
	// matching SPEC §6 "limits" table.
	string CreateOwnedSession(const string &session_id, const string &owner_principal_id);

	// Look up a session by id, gated on ownership. Returns nullptr if
	// EITHER (a) no such session OR (b) session exists but
	// session.owner_principal_id != principal_id. The two cases are
	// indistinguishable to the caller, by design (SPEC §6 line 637-643).
	// `principal_id` empty matches sessions with empty owner (legacy
	// path); a non-empty principal_id requires exact match.
	shared_ptr<FlockSession> LookupOwnedSession(const string &session_id, const string &principal_id);

	// Destroy a session by id, gated on ownership. Returns false if EITHER
	// not-found OR wrong-owner. Idempotent for the not-found case.
	bool DestroyOwnedSession(const string &session_id, const string &principal_id);

	// Destroy ALL sessions owned by `principal_id`. Returns the count of
	// sessions destroyed (may be 0). Used by /auth/logout?destroy_sessions=true.
	// Sessions with empty owner_principal_id are NOT touched (they belong
	// to the legacy QuackHandlers path which manages its own lifecycle).
	idx_t DestroyAllOwnedBy(const string &principal_id);

	// Generate a fresh CSPRNG-backed 16-byte (128-bit) session id, hex-encoded
	// (32 chars). Lazy-initializes the RNG via db.GetEncryptionUtil() on first
	// call — which in DuckDB's default configuration auto-loads httpfs and
	// routes through its OpenSSL-backed EncryptionUtil (the only alternatives
	// are deliberately insecure mbedTLS Mersenne-Twister fallbacks gated behind
	// force_mbedtls_unsafe='true', or outright failure in read-only mode). So
	// this is practically OpenSSL's RAND_bytes via httpfs, even though we
	// don't link OpenSSL for this particular call. See the parallel comment
	// on AuthManager::GenerateRandomToken in src/include/flock_auth.hpp.
	// Thread-safe; the rng_mutex guards lazy-init only — actual byte
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
