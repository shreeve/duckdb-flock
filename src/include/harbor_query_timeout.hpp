#pragma once

// PR-7b — `harbor_query_timeout_s` runtime enforcement scaffolding.
//
// The setting `harbor_query_timeout_s` (UBIGINT, default 0 = no limit)
// caps the wall-clock duration of every Connection::Execute path:
// /sql, /quack PREPARE/APPEND/FETCH, /ddb/run, and the admin transient
// queries (/tables, /schema, /checkpoint). When the deadline elapses
// the active session is interrupted via Connection::Interrupt() and
// the catch path reports HTTP 504 with errorCode "QUERY_TIMEOUT" (or,
// for streaming /sql NDJSON, a final `{"type":"error","code":"QUERY_TIMEOUT"}`
// line on the existing 200-OK response).
//
// Two enforcement vehicles, both designed in GPT-5.5 round 21:
//
//   1. SessionManager-tracked sessions use a single SWEEPER thread per
//      SessionManager (250ms tick). The sweeper iterates the active
//      map under the map lock, copies shared_ptrs, releases the lock,
//      and inspects each session's `query_deadline_ms` against the
//      monotonic clock. Sessions past the deadline have
//      `timed_out_generation` set to their CURRENT `query_generation`
//      (a CAS-style write), then `Connection::Interrupt()` is called
//      WITHOUT taking the per-session lock (Interrupt is concurrency-
//      safe; the lock is held by the in-flight Execute we're trying
//      to interrupt — taking it would deadlock).
//
//      The generation counter is the load-bearing race-fix: without it
//      the sweeper could observe a finished-then-restarted query and
//      stale-interrupt the NEXT query. With it, the
//      QueryExecutionGuard destructor only honors the timed-out
//      classification when `timed_out_generation == my_generation`.
//
//   2. Ephemeral connections — fresh /sql Connections that aren't
//      registered with SessionManager, plus the transient Connections
//      AdminHandlers spins up for /tables / /schema / /checkpoint
//      and any future UI surface that bypasses the pool — are
//      instead guarded by a per-request RAII watchdog. The watchdog
//      thread waits on a condition_variable until either the deadline
//      elapses (in which case it sets a flag and calls Interrupt())
//      or the destructor signals `done` and joins. No detached
//      threads; clean shutdown on every code path.
//
// Both paths converge on the same error classification: the catch
// block reads `interrupt_cause` (or, for ephemerals, the watchdog's
// `timed_out` flag) to distinguish QUERY_TIMEOUT from USER_CANCEL /
// DISCONNECT / generic SQL exceptions.
//
// THIS HEADER currently stubs the API surface. Implementation lands
// in PR-7b/2 (sweeper) and PR-7b/3 (watchdog) in this same branch.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "duckdb/common/common.hpp"
#include "harbor_session.hpp"

namespace duckdb {

class Connection;
class DatabaseInstance;

// Convert the `harbor_query_timeout_s` setting into a monotonic deadline
// `ms-since-steady_clock-epoch`. Returns 0 when timeout is disabled
// (setting is 0, missing, or unreadable). Used by every call site that
// constructs a QueryExecutionGuard or QueryTimeoutWatchdog.
int64_t ResolveQueryDeadlineMs(DatabaseInstance &db);

// Convenience: read `harbor_query_timeout_s` as a uint64 (seconds).
// Returns 0 on missing / unreadable / non-UBIGINT. Used by handlers
// that need the raw timeout value (e.g. for logging) before wrapping.
uint64_t ReadQueryTimeoutSeconds(DatabaseInstance &db);

// PR-7b — RAII guard for queries running on a SessionManager-tracked
// HarborSession. Constructor:
//   - increments `session.query_generation` and remembers `my_generation`,
//   - sets `session.last_query` (under per-session lock — caller must
//     hold session.lock when constructing this guard),
//   - sets `session.query_in_flight = true`,
//   - sets `session.query_deadline_ms` from `timeout_seconds` (0 = no deadline),
//   - clears any prior `interrupt_cause` to NONE.
//
// Destructor:
//   - reads `session.timed_out_generation` to detect a TIMEOUT race fairly:
//     if it equals `my_generation`, this query was the one cancelled
//     and `TimedOut()` returns true; otherwise the timeout was meant
//     for a different generation (or never fired) and we report false.
//   - clears `query_in_flight`, `query_deadline_ms` only if they still
//     belong to `my_generation` (defense against a sweeper that ran
//     between Execute completion and destructor entry).
//
// The catch path inspects `TimedOut()` to map a generic interrupt
// exception to errorCode QUERY_TIMEOUT vs another cause.
class QueryExecutionGuard {
public:
	QueryExecutionGuard(HarborSession &session, const string &sql, uint64_t timeout_seconds);
	~QueryExecutionGuard();

	QueryExecutionGuard(const QueryExecutionGuard &) = delete;
	QueryExecutionGuard &operator=(const QueryExecutionGuard &) = delete;

	// True iff the sweeper marked this guard's generation as timed out.
	// Safe to call from the catch path; reads atomics only.
	bool TimedOut() const;

	// Read whatever cause the cancellation was attributed to (TIMEOUT
	// from the sweeper, USER_CANCEL from /sql/cancel or /interrupt,
	// DISCONNECT from a client-disconnect handler). Returns NONE if
	// no interrupt fired or the cause didn't match this generation.
	InterruptCause Cause() const;

private:
	HarborSession &session;
	uint64_t my_generation;
};

// PR-7b — RAII watchdog for queries running on a transient Connection
// that is NOT in the SessionManager pool (ephemeral /sql, transient
// admin queries, future UI variants). Spawns ONE std::thread per
// constructed instance. The thread waits on `cv` for `timeout_seconds`
// or until the destructor signals `done`. On timeout: sets `timed_out`
// true and calls `connection.Interrupt()`. Destructor signals `done`,
// notifies cv, and joins the thread cleanly — never detaches.
//
// `timeout_seconds == 0` is a no-op: no thread is spawned; `TimedOut()`
// always returns false. This keeps the zero-timeout case overhead-free.
class QueryTimeoutWatchdog {
public:
	QueryTimeoutWatchdog(Connection &connection, uint64_t timeout_seconds);
	~QueryTimeoutWatchdog();

	QueryTimeoutWatchdog(const QueryTimeoutWatchdog &) = delete;
	QueryTimeoutWatchdog &operator=(const QueryTimeoutWatchdog &) = delete;

	// True iff the watchdog thread fired Interrupt() before the
	// destructor signalled completion. Safe from any thread.
	bool TimedOut() const;

private:
	Connection &connection;
	std::thread thread;
	std::mutex mutex;
	std::condition_variable cv;
	bool done = false;
	std::atomic<bool> timed_out {false};
	bool active = false; // false when timeout_seconds == 0
};

} // namespace duckdb
