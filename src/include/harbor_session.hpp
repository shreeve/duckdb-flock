#pragma once

// SessionManager and HarborSession — the per-process DB session pool.
//
// Lifted from upstream QuackServer's connection-pool members
// (active_connections + session_id_rng) so that PR-3+ handler subsystems
// (UiHandlers, SqlHandlers) can share the same pool with QuackHandlers.
//
// PR-2 keeps the existing semantics (no idle TTL, no per-principal
// scoping) — those land in PR-3/PR-4 with the auth-cookie/principal
// model. The struct shape of HarborSession is identical to upstream
// QuackConnection; only the name changes to align with SPEC §6
// vocabulary.

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

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
// PR-7b: classify why a session's in-flight query was cancelled, so
// the catch-side error mapping can report QUERY_TIMEOUT (HTTP 504 or
// mid-stream NDJSON line) distinctly from a user-issued /sql/cancel,
// client-disconnect, or unsolicited DuckDB exception. Per round-21
// review: DuckDB exception text is unreliable as the only signal.
//
// Stored as an `std::atomic<uint8_t>` rather than `std::atomic<enum>`
// for portability (GCC/Clang/MSVC all guarantee lock-free atomic<u8>).
enum class InterruptCause : uint8_t {
	NONE = 0,         // no interrupt fired (or already cleared)
	TIMEOUT = 1,      // sweeper or RAII watchdog hit harbor_query_timeout_s
	USER_CANCEL = 2,  // POST /sql/cancel or POST /interrupt
	DISCONNECT = 3,   // client closed connection mid-stream / quack DISCONNECT
};

struct HarborSession {
	explicit HarborSession(string session_id_p);
	HarborSession(string session_id_p, string owner_principal_id_p);
	~HarborSession();

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

	// PR-6: instrumentation surfaced by AdminHandlers' GET /sessions.
	//
	// `created_at` is set in the ctor and never mutated. `last_query`
	// is guarded by the per-session `lock`; handlers (SqlHandlers,
	// QuackHandlers PREPARE/APPEND, UiHandlers /ddb/run) update it
	// just before issuing Connection::Execute. `query_in_flight` is
	// flipped true around Execute and false on completion (or on
	// exception via a scope guard). It is std::atomic<bool> so
	// SessionManager::Snapshot() can read it without acquiring the
	// per-session mutex.
	const std::chrono::steady_clock::time_point created_at;
	string last_query;                       // guarded by `lock`
	std::atomic<bool> query_in_flight {false};

	// PR-7b: query-timeout enforcement state. Per round-21 review
	// (GPT-5.5), naive sweeper-based interrupts have a fatal race —
	// a stale interrupt can hit the next query after the previous one
	// completed and a new one started. Generation-versioned state
	// closes that race:
	//
	// `query_generation`: monotonically incremented by the
	//                     QueryExecutionGuard ctor each time a new
	//                     query starts on this session. Read by the
	//                     sweeper to identify which generation it's
	//                     about to interrupt; written to
	//                     `timed_out_generation` BEFORE calling
	//                     Connection::Interrupt().
	//
	// `query_deadline_ms`: monotonic clock deadline (ms since
	//                      steady_clock epoch); 0 means no timeout.
	//                      Read by the sweeper.
	//
	// `timed_out_generation`: set by the sweeper to the generation
	//                          it interrupted. Read by the
	//                          QueryExecutionGuard destructor (and
	//                          the catch path) to classify whether
	//                          THIS query was the one timed out;
	//                          if `timed_out_generation == my_generation`,
	//                          the interrupt was a TIMEOUT, otherwise
	//                          some other cause won the race.
	//
	// `interrupt_cause`: optional separate signal for USER_CANCEL /
	//                    DISCONNECT, set by their respective callers
	//                    just before calling Interrupt(). The catch
	//                    path reads this to map to the right
	//                    errorCode. NONE = no cause set; cleared by
	//                    QueryExecutionGuard at completion.
	std::atomic<uint64_t> query_generation {0};
	std::atomic<int64_t> query_deadline_ms {0};
	std::atomic<uint64_t> timed_out_generation {0};
	std::atomic<uint8_t> interrupt_cause {static_cast<uint8_t>(InterruptCause::NONE)};
};

// PR-6 — value snapshot of a HarborSession's instrumentation fields,
// produced by SessionManager::Snapshot() for /sessions admin output.
// Decoupled from HarborSession so the JSON encoder can iterate without
// holding any mutex (the snapshot copy already paid the cost).
struct SessionSnapshot {
	string session_id;
	string owner_principal_id;
	std::chrono::steady_clock::time_point created_at;
	string last_query;
	bool last_query_truncated = false;
	bool query_in_flight = false;
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
	shared_ptr<HarborSession> GetConnection(const string &session_id);

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
	// session count would exceed `harbor_max_sessions` (default 1024).
	// Note: the limit applies to the GLOBAL active count, not per-principal,
	// matching SPEC §6 "limits" table.
	string CreateOwnedSession(const string &session_id, const string &owner_principal_id);

	// Look up a session by id, gated on ownership. Returns nullptr if
	// EITHER (a) no such session OR (b) session exists but
	// session.owner_principal_id != principal_id. The two cases are
	// indistinguishable to the caller, by design (SPEC §6 line 637-643).
	// `principal_id` empty matches sessions with empty owner (legacy
	// path); a non-empty principal_id requires exact match.
	shared_ptr<HarborSession> LookupOwnedSession(const string &session_id, const string &principal_id);

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
	// on AuthManager::GenerateRandomToken in src/include/harbor_auth.hpp.
	// Thread-safe; the rng_mutex guards lazy-init only — actual byte
	// generation is single-call into DuckDB's encryption util.
	string GenerateSessionId();

	// Number of currently-active sessions. Snapshot value; may change between
	// the call and any subsequent GetConnection.
	idx_t ActiveCount();

	// PR-6 — snapshot of session instrumentation for AdminHandlers'
	// GET /sessions. Lock ordering (per round-18 review):
	//   1. Take active_mutex; copy out shared_ptr<HarborSession>'s
	//      and release active_mutex.
	//   2. For each session, briefly take session->lock to copy
	//      `last_query` (which is mutated under that lock).
	//   3. Read query_in_flight via std::atomic — no lock needed.
	//   4. `last_query` is truncated to `last_query_max_chars`; the
	//      `last_query_truncated` flag indicates whether truncation
	//      happened. Pass 0 to skip including last_query entirely.
	std::vector<SessionSnapshot> Snapshot(idx_t last_query_max_chars = 200);

	// PR-6 — interrupt the in-flight query (if any) on the named session.
	// Returns true if the session was found and Connection::Interrupt()
	// was called; false if the session id was unknown. Does NOT acquire
	// the per-session mutex (Connection::Interrupt is documented as
	// concurrency-safe — it sets the executor's interrupt flag, which
	// the in-flight Execute checks at safe yield points).
	//
	// Used by /interrupt (admin) and /sql/cancel (admin). Both are
	// authz-gated by the caller; SessionManager itself enforces no
	// principal ownership check — admin scope deliberately spans
	// principals.
	bool InterruptSession(const string &session_id);

private:
	weak_ptr<DatabaseInstance> db;

	std::mutex active_mutex;
	unordered_map<string, shared_ptr<HarborSession>> active;

	std::mutex rng_mutex;
	shared_ptr<EncryptionState> rng;
};

} // namespace duckdb
