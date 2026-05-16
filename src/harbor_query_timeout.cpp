// PR-7b — `harbor_query_timeout_s` runtime enforcement scaffolding.
//
// THIS FILE IS PR-7b FOUNDATION ONLY (committed as WIP).
//
// What's implemented tonight:
//   - ResolveQueryDeadlineMs / ReadQueryTimeoutSeconds setting readers.
//   - QueryExecutionGuard (full RAII; sets and clears HarborSession
//     atomics with generation discipline).
//   - QueryTimeoutWatchdog with the RAII shape (ctor/dtor join cleanly)
//     and the condition_variable wait — fires Interrupt() on timeout.
//
// What lands in the next commit on this same branch:
//   - SessionManager sweeper thread (250ms tick) that walks the active
//     session map and writes timed_out_generation + Interrupt().
//   - Wiring: SqlHandlers::HandleSql, QuackHandlers PREPARE/APPEND/FETCH,
//     UiHandlers::HandleRun, AdminHandlers /tables/schema/checkpoint
//     each construct a guard or watchdog around their Execute calls.
//   - Catch-path classification: read InterruptCause + TimedOut() to
//     map to errorCode QUERY_TIMEOUT (HTTP 504 pre-response, mid-stream
//     `{"type":"error","code":"QUERY_TIMEOUT"}` for streaming /sql).
//   - Tests: golden-sql-roundtrip.sh extended with timeout scenarios
//     across sessionful + ephemeral + streaming + one-shot paths.
//
// Until those land, the constructed guards / watchdogs are
// LIBRARY-COMPILABLE but unwired — no caller invokes them yet, so
// behavior is unchanged from PR-7a.

#include "harbor_query_timeout.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

uint64_t ReadQueryTimeoutSeconds(DatabaseInstance &db) {
	Value setting_val;
	auto &config = DBConfig::GetConfig(db);
	if (!config.TryGetCurrentSetting("harbor_query_timeout_s", setting_val) || setting_val.IsNull() ||
	    setting_val.type().id() != LogicalTypeId::UBIGINT) {
		return 0;
	}
	try {
		return setting_val.GetValue<uint64_t>();
	} catch (...) {
		return 0;
	}
}

int64_t ResolveQueryDeadlineMs(DatabaseInstance &db) {
	auto seconds = ReadQueryTimeoutSeconds(db);
	if (seconds == 0) {
		return 0; // no deadline
	}
	auto now = std::chrono::steady_clock::now();
	auto deadline = now + std::chrono::seconds(static_cast<int64_t>(seconds));
	return std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count();
}

// -- QueryExecutionGuard ----------------------------------------------------
//
// Caller MUST hold session.lock when constructing this guard. The guard
// mutates `last_query` under that lock; query_generation /
// query_deadline_ms / query_in_flight are atomics so they don't need the
// lock, but ordering matters: we set generation BEFORE deadline, BEFORE
// in_flight, so a concurrent sweeper either sees no in-flight (skip) or
// observes a fully-armed deadline.

QueryExecutionGuard::QueryExecutionGuard(HarborSession &session_p, const string &sql, uint64_t timeout_seconds)
    : session_ptr(&session_p) {
	// Increment the generation FIRST. Sweeper iterations that captured
	// the prior generation can no longer mark this query as timed-out;
	// only sweeper iterations that observe THIS generation count.
	my_generation = session_ptr->query_generation.fetch_add(1, std::memory_order_acq_rel) + 1;

	// Caller holds session.lock; safe to mutate last_query.
	session_ptr->last_query = sql;

	// Reset prior cause to NONE so a stale USER_CANCEL/DISCONNECT
	// signal from a previous query doesn't leak into this one.
	session_ptr->interrupt_cause.store(static_cast<uint8_t>(InterruptCause::NONE), std::memory_order_release);

	// Compute and arm deadline (if timeout is enabled).
	int64_t deadline_ms = 0;
	if (timeout_seconds > 0) {
		auto now = std::chrono::steady_clock::now();
		auto when = now + std::chrono::seconds(static_cast<int64_t>(timeout_seconds));
		deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
	}
	session_ptr->query_deadline_ms.store(deadline_ms, std::memory_order_release);

	// Finally flip in_flight true. Sweeper observes (in_flight == true
	// AND deadline_ms != 0 AND now > deadline_ms) before interrupting.
	session_ptr->query_in_flight.store(true, std::memory_order_release);
}

QueryExecutionGuard::~QueryExecutionGuard() {
	if (!session_ptr) {
		return; // moved-from
	}
	// Clear deadline + in_flight. Both atomics; the sweeper that
	// runs between this clear and the next QueryExecutionGuard's ctor
	// observes deadline=0 and skips.
	session_ptr->query_deadline_ms.store(0, std::memory_order_release);
	session_ptr->query_in_flight.store(false, std::memory_order_release);
	// Do NOT reset query_generation; it must monotonically grow so
	// the next guard's ctor produces a strictly-greater value.
	// Do NOT touch timed_out_generation; the catch-path reader needs
	// to observe whether it equals my_generation.
}

QueryExecutionGuard::QueryExecutionGuard(QueryExecutionGuard &&other) noexcept
    : session_ptr(other.session_ptr), my_generation(other.my_generation) {
	other.session_ptr = nullptr;
}

QueryExecutionGuard &QueryExecutionGuard::operator=(QueryExecutionGuard &&other) noexcept {
	if (this != &other) {
		// Run our own destructor logic first (release any state we currently own).
		if (session_ptr) {
			session_ptr->query_deadline_ms.store(0, std::memory_order_release);
			session_ptr->query_in_flight.store(false, std::memory_order_release);
		}
		session_ptr = other.session_ptr;
		my_generation = other.my_generation;
		other.session_ptr = nullptr;
	}
	return *this;
}

bool QueryExecutionGuard::TimedOut() const {
	if (!session_ptr) {
		return false;
	}
	return session_ptr->timed_out_generation.load(std::memory_order_acquire) == my_generation;
}

InterruptCause QueryExecutionGuard::Cause() const {
	if (!session_ptr) {
		return InterruptCause::NONE;
	}
	if (TimedOut()) {
		return InterruptCause::TIMEOUT;
	}
	auto raw = session_ptr->interrupt_cause.load(std::memory_order_acquire);
	return static_cast<InterruptCause>(raw);
}

// -- QueryTimeoutWatchdog ---------------------------------------------------

QueryTimeoutWatchdog::QueryTimeoutWatchdog(Connection &connection_p, uint64_t timeout_seconds)
    : connection(connection_p) {
	if (timeout_seconds == 0) {
		// No-op watchdog. No thread spawned; TimedOut() always false.
		// Caller can construct one unconditionally and pay zero cost
		// when the timeout setting is disabled.
		active = false;
		return;
	}
	active = true;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(static_cast<int64_t>(timeout_seconds));
	thread = std::thread([this, deadline] {
		// Brace-init avoids the most-vexing-parse MSVC hits when a
		// member named `mutex` is in scope; the member rename to
		// `mu_` makes the parens form work too, but the brace-init
		// is the more-defensive idiom and we keep it for clarity.
		std::unique_lock<std::mutex> lk{mu_};
		// wait_until's predicate form contract: returns true iff the
		// predicate is true at exit. If the destructor signalled
		// `done` (either before our wait or by notifying the cv),
		// the predicate returns true and we skip the Interrupt.
		// If the deadline elapses without `done` being set, the
		// predicate returns false → we fire Interrupt.
		const bool predicate_satisfied = cv.wait_until(lk, deadline, [this] { return done; });
		if (predicate_satisfied) {
			return; // destructor woke us — query finished cleanly
		}
		// Deadline elapsed with `done` still false. Mark the
		// timed-out flag and call Interrupt() WITHOUT taking
		// session locks (Connection::Interrupt is concurrency-safe).
		timed_out.store(true, std::memory_order_release);
		// Drop the watchdog mutex before Interrupt() so the destructor
		// can grab it to set done=true even if Interrupt() is slow.
		lk.unlock();
		try {
			connection.Interrupt();
		} catch (...) {
			// Connection::Interrupt() doesn't throw by contract,
			// but swallow defensively — letting an exception escape
			// here would std::terminate (we're in a thread).
		}
	});
}

QueryTimeoutWatchdog::~QueryTimeoutWatchdog() {
	if (!active) {
		return;
	}
	{
		std::lock_guard<std::mutex> lk{mu_};
		done = true;
	}
	cv.notify_one();
	if (thread.joinable()) {
		thread.join();
	}
}

bool QueryTimeoutWatchdog::TimedOut() const {
	return timed_out.load(std::memory_order_acquire);
}

} // namespace duckdb
