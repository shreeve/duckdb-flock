#pragma once

// HarborHttpServer — owns the cpp-httplib Server, listener thread, and
// shared subsystems (SessionManager, AuthManager). PR-2's central
// architectural piece.
//
// PR-2 invariant (enforced by a CI grep guard):
//
//   grep -R "duckdb_httplib::Server" src | grep -v harbor_http_server
//
// must produce no matches. Exactly one duckdb_httplib::Server instance
// exists in the process; only this file constructs or owns it. Handler
// subsystems (QuackHandlers, AdminHandlers, future UiHandlers /
// SqlHandlers) get a reference via Server() and register their routes
// against it — they MUST NOT call server.stop() themselves.
//
// HarborServerState is a process-static singleton holding the running
// HarborHttpServer (single-server-per-process per SPEC §2). Both the
// "quack" and "harbor" StorageExtension keys point at the same global
// instance — Find("quack").state == Find("harbor").state ==
// &HarborServerState::Global() — so stock-quack ATTACH callers and
// harbor-aware tooling see the same lifecycle.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"

#include "quack_uri.hpp"

// PR-3: migrate from duckdb_httplib (plain) to duckdb_httplib_openssl
// (the OpenSSL-enabled cpp-httplib build) so UI handlers — whose
// upstream code uses the openssl namespace — can register routes
// against our shared server. The two namespaces produce separate C++
// types (not interchangeable); the openssl variant has the same API
// surface as plain plus HTTPS support. PR-2's "single httplib::Server
// owner" invariant still holds — we just own the openssl variant
// instead. The CI grep guard at .github/workflows/architecture-guard.yml
// now matches both `duckdb_httplib::Server` and
// `duckdb_httplib_openssl::Server`.
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

namespace duckdb {

class DatabaseInstance;
class ClientContext;
class SessionManager;
class AuthManager;
class QuackHandlers;
class AdminHandlers;
class AuthHandlers;
class SqlHandlers;
namespace ui {
class UiHandlers;
}

// HarborHttpServer owns the cpp-httplib Server, listener thread, and
// per-server subsystems. Construction is ctor → Bind() →
// RegisterBuiltinHandlers() → StartListening(). Shutdown is
// StopAccepting() (close socket, safe from any thread) followed by
// Close() (drain in-flight workers, join listener; NOT safe from a
// worker thread).
class HarborHttpServer {
public:
	enum class State : uint8_t {
		CONSTRUCTED, // ctor done; Bind() not yet called
		BOUND,       // socket bound; routes can be registered; not yet listening
		LISTENING,   // listener thread running; routes frozen
		CLOSING,     // StopAccepting called; draining in-flight requests
		CLOSED,      // Close() done; safe to destroy
	};

	// `unauthenticated` (v0.2): if true, all auth-bearing routes accept
	// requests without a credential and assign the synthetic
	// `harbor.local-dev` principal. Triggered exclusively by
	// `harbor_serve(uri, token := NULL)` on a loopback bind. See SPEC §7.
	HarborHttpServer(weak_ptr<DatabaseInstance> db, QuackUri uri, string token, bool unauthenticated);
	~HarborHttpServer();

	HarborHttpServer(const HarborHttpServer &) = delete;
	HarborHttpServer &operator=(const HarborHttpServer &) = delete;

	// Synchronously bind the listening socket. Throws IOException if bind
	// fails (EADDRINUSE, permission denied, invalid host/port). Configures
	// the cpp-httplib thread pool (128 workers), keep-alive (count=128,
	// timeout=10s), and TCP_NODELAY — config moves here from upstream
	// HttpQuackServer's ctor. Transitions CONSTRUCTED → BOUND.
	void Bind();

	// Construct the built-in handler subsystems (QuackHandlers,
	// AdminHandlers, UiHandlers) and register their routes against the
	// server. Must be called between Bind() and StartListening().
	// Concrete handler fields stored on `this` so route lambdas
	// (which capture handler `this`) outlive request execution.
	//
	// PR-3+: takes a ClientContext for per-handler settings lookup
	// (UiHandlers reads ui_remote_url, ui_polling_interval at construction).
	void RegisterBuiltinHandlers(ClientContext &context);

	// Spawn the listener thread (calls listen_after_bind on the bound
	// socket). Returns immediately; the listener runs in the background.
	// Transitions BOUND → LISTENING.
	void StartListening();

	// Close the listening socket without draining workers or joining
	// the listener thread. Idempotent. Safe to call from any thread
	// including a request-handler thread (does not wait on the httplib
	// worker pool, which would deadlock when called from a worker).
	// Transitions LISTENING → CLOSING (or no-op in CLOSING/CLOSED).
	void StopAccepting();

	// StopAccepting() + ShutdownHandlers() + drain in-flight request
	// handlers + join listener thread. MUST NOT be called from a
	// request-handler thread — joining the listener while holding a
	// worker thread deadlocks through httplib's listen-loop teardown
	// chain. Transitions any state → CLOSED.
	void Close();

	// Tell each handler subsystem to release resources that aren't
	// request-scoped — long-running threads (UiHandlers' Watcher) and
	// blocking SSE consumers (UiHandlers' EventDispatcher::Close()
	// which wakes /localEvents WaitEvent calls). MUST be called BEFORE
	// DrainActiveRequests, because those long-running threads aren't
	// counted in active_requests; they'd block the drain forever
	// otherwise. Idempotent. Called by Close() in the right order; no
	// reason to call directly.
	void ShutdownHandlers();

	// Reference to the shared httplib server. Handler subsystems pass
	// this to their Register() methods. Asserts state == BOUND
	// (registering a route while the listener is running is undefined
	// per cpp-httplib).
	duckdb_httplib_openssl::Server &Server();

	// Accessors. Safe to call concurrently with the listener thread;
	// these read immutable state set in the ctor or Bind().
	const QuackUri &ListenUri() const {
		return uri;
	}
	const string &Token() const {
		return token;
	}
	// True iff `harbor_serve` was invoked with `token := NULL` on a
	// loopback bind, opening the server to unauthenticated local
	// connections. Snapshotted at server-start time; never changes
	// while the server is running.
	bool IsUnauthenticated() const {
		return unauthenticated;
	}
	std::chrono::steady_clock::time_point StartedAt() const {
		return started_at;
	}
	weak_ptr<DatabaseInstance> Database() const {
		return db;
	}

	// Subsystems borrowed by reference from handler ctors. Both are
	// constructed in HarborHttpServer's ctor so they outlive every handler
	// that captures references to them.
	SessionManager &Sessions();
	AuthManager &Auth();

	// RAII helper used by handler route lambdas to participate in the
	// shutdown drain. Increment on entry, decrement on exit; Close()
	// waits for the count to reach zero before destroying handlers.
	//
	// Usage in a handler lambda:
	//   server->Server().Post("/quack", [h](req, res, reader) {
	//     HarborHttpServer::ActiveRequestGuard guard(h->server);
	//     // ... handle request ...
	//   });
	class ActiveRequestGuard {
	public:
		explicit ActiveRequestGuard(HarborHttpServer &srv);
		~ActiveRequestGuard();

		ActiveRequestGuard(const ActiveRequestGuard &) = delete;
		ActiveRequestGuard &operator=(const ActiveRequestGuard &) = delete;

	private:
		HarborHttpServer &srv;
	};

private:
	weak_ptr<DatabaseInstance> db;
	QuackUri uri;
	string token;
	bool unauthenticated;
	std::chrono::steady_clock::time_point started_at;

	std::mutex state_mu;
	State state = State::CONSTRUCTED;

	unique_ptr<duckdb_httplib_openssl::Server> server;
	std::thread listen_thread; // .joinable() == false until StartListening()

	std::atomic<idx_t> active_requests {0};
	std::mutex drain_mu;
	std::condition_variable cv_no_active;

	unique_ptr<SessionManager> sessions;
	unique_ptr<AuthManager> auth;

	// Concrete handler ownership. Route lambdas registered against the
	// httplib server capture `quack_handlers.get()` / `admin_handlers.get()` /
	// `ui_handlers.get()` via `this`. These pointers stay valid until
	// ~HarborHttpServer (which requires Close() to have drained workers first).
	unique_ptr<QuackHandlers> quack_handlers;
	unique_ptr<AdminHandlers> admin_handlers;
	unique_ptr<AuthHandlers> auth_handlers;
	unique_ptr<SqlHandlers> sql_handlers;
	unique_ptr<ui::UiHandlers> ui_handlers;

	// Listener-thread entry. Catches everything so an exception in the
	// listener never escapes (which would call std::terminate and abort
	// the host process).
	static void ListenThreadMain(HarborHttpServer *srv);

	// Wait for active_requests to reach 0 or for the timeout. Returns
	// true if drained cleanly, false if timed out.
	bool DrainActiveRequests(std::chrono::seconds timeout);
};

// Process-static singleton holding the (single) running HarborHttpServer
// per SPEC §2. Generation counter pattern (per GPT-5.5 round 5 catch
// #12) so that Wait() can correctly distinguish "the server I was
// waiting on stopped" from "a new server started after my wait began".
class HarborServerState {
public:
	// The single shared state for the entire process. Both the "quack"
	// and "harbor" StorageExtension keys point at this same object.
	static HarborServerState &Global();

	HarborServerState(const HarborServerState &) = delete;
	HarborServerState &operator=(const HarborServerState &) = delete;

	// Start the server. Throws InvalidInputException if a server is
	// already running (single-server-per-process). Increments the
	// generation counter and clears stop_requested for the new run.
	//
	// PR-3+: takes a ClientContext for handler-construction settings
	// lookup (UiHandlers reads ui_remote_url etc.).
	void Start(ClientContext &context, weak_ptr<DatabaseInstance> db, QuackUri uri, string token,
	           bool unauthenticated);

	// Stop the running server. Returns false if none was running on
	// the given URI. Records stopped_generation == current generation
	// and notifies any threads blocked in Wait().
	bool Stop(const QuackUri &uri);

	// Block the caller until the current-generation server stops or
	// the process receives SIGTERM/SIGINT. Returns true on either
	// clean-stop path; throws InvalidInputException if no server is
	// running at the moment Wait() is called. Generation-aware: if
	// Start/Stop/Start happens between two Wait calls, each Wait
	// returns when ITS generation stops.
	bool Wait();

	// True iff a server is currently running (any URI).
	bool IsRunning();

	// For introspection (quack_server_list etc). The functor runs while
	// state_mu is held — caller MUST NOT block in fn or capture the
	// server reference beyond the call.
	void WithCurrent(const std::function<void(HarborHttpServer &)> &fn);

private:
	HarborServerState() = default;
	~HarborServerState() = default;

	mutable std::mutex state_mu;
	std::condition_variable cv;

	unique_ptr<HarborHttpServer> server;
	uint64_t generation = 0;
	uint64_t stopped_generation = 0;
};

} // namespace duckdb
