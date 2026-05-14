#include "flock_http_server.hpp"

#include "admin_handlers.hpp"
#include "flock_auth.hpp"
#include "flock_session.hpp"
#include "quack_server.hpp"  // QuackHandlers
#include "ui_handlers.hpp"   // ui::UiHandlers

#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

namespace {

// cpp-httplib worker pool size. Inherited from upstream HttpQuackServer:
// each keep-alive connection holds a worker for its lifetime, so we
// need enough workers to handle catalog clients + scan-thread clients
// simultaneously without deadlock.
constexpr int kHttplibWorkers = 128;
constexpr int kHttplibKeepAliveCount = 128;
constexpr int kHttplibKeepAliveTimeoutSec = 10;

// Default drain timeout if the `flock_stop_drain_timeout_s` setting
// isn't configured. Matches SPEC §9.
constexpr int kDefaultDrainTimeoutSec = 30;

int GetDrainTimeoutSec(weak_ptr<DatabaseInstance> &db) {
	auto db_locked = db.lock();
	if (!db_locked) {
		return kDefaultDrainTimeoutSec;
	}
	Value setting_val;
	auto &config = DBConfig::GetConfig(*db_locked);
	if (!config.TryGetCurrentSetting("flock_stop_drain_timeout_s", setting_val)) {
		return kDefaultDrainTimeoutSec;
	}
	if (setting_val.IsNull()) {
		return kDefaultDrainTimeoutSec;
	}
	try {
		auto v = setting_val.GetValue<uint64_t>();
		if (v > static_cast<uint64_t>(INT32_MAX)) {
			return INT32_MAX;
		}
		return static_cast<int>(v);
	} catch (...) {
		return kDefaultDrainTimeoutSec;
	}
}

} // namespace

// -- ActiveRequestGuard ---------------------------------------------------

FlockHttpServer::ActiveRequestGuard::ActiveRequestGuard(FlockHttpServer &srv_p) : srv(srv_p) {
	srv.active_requests.fetch_add(1, std::memory_order_acq_rel);
}

FlockHttpServer::ActiveRequestGuard::~ActiveRequestGuard() {
	auto remaining = srv.active_requests.fetch_sub(1, std::memory_order_acq_rel);
	if (remaining == 1) {
		// We were the last in-flight request. Wake any thread blocked
		// in DrainActiveRequests().
		std::lock_guard<std::mutex> lock(srv.drain_mu);
		srv.cv_no_active.notify_all();
	}
}

// -- FlockHttpServer ------------------------------------------------------

FlockHttpServer::FlockHttpServer(weak_ptr<DatabaseInstance> db_p, QuackUri uri_p, string token_p)
    : db(std::move(db_p)), uri(std::move(uri_p)), token(std::move(token_p)),
      started_at(std::chrono::steady_clock::now()) {
	sessions = make_uniq<SessionManager>(db);
	auth = make_uniq<AuthManager>(db, token);
}

FlockHttpServer::~FlockHttpServer() {
	try {
		Close();
	} catch (...) {
		// dtors must not throw; swallow.
	}
}

void FlockHttpServer::Bind() {
	std::lock_guard<std::mutex> lock(state_mu);
	if (state != State::CONSTRUCTED) {
		throw InvalidInputException("FlockHttpServer::Bind called in wrong state");
	}

	server = make_uniq<duckdb_httplib_openssl::Server>();

	// Each keep-alive connection holds a server thread for its lifetime;
	// we need enough threads to handle all concurrent keep-alive
	// connections (catalog clients + scan-thread clients) without
	// deadlock.
	server->new_task_queue = [] { return new duckdb_httplib_openssl::ThreadPool(kHttplibWorkers); };
	server->set_keep_alive_max_count(kHttplibKeepAliveCount);
	server->set_keep_alive_timeout(kHttplibKeepAliveTimeoutSec);
	server->set_tcp_nodelay(true);

	if (!server->is_valid()) {
		throw IOException("Failed to instantiate flock HTTP server at %s / %s", uri.Uri(), uri.Http());
	}

	// Synchronous bind so that bind() failures (e.g. EADDRINUSE) propagate
	// to the caller of flock_serve / quack_serve.
	if (!server->bind_to_port(uri.Host(), uri.Port())) {
		throw IOException("Failed to bind flock HTTP server to %s (address in use, permission denied, or invalid host/port)",
		                  uri.Http());
	}

	state = State::BOUND;
}

duckdb_httplib_openssl::Server &FlockHttpServer::Server() {
	std::lock_guard<std::mutex> lock(state_mu);
	if (state != State::BOUND) {
		throw InvalidInputException("FlockHttpServer::Server() called outside the BOUND state — "
		                            "register routes between Bind() and StartListening()");
	}
	return *server;
}

void FlockHttpServer::RegisterBuiltinHandlers(ClientContext &context) {
	{
		std::lock_guard<std::mutex> lock(state_mu);
		if (state != State::BOUND) {
			throw InvalidInputException("FlockHttpServer::RegisterBuiltinHandlers called in wrong state");
		}
	}

	// Order matters: cpp-httplib resolves routes in registration order.
	// UiHandlers' GET /.* catch-all MUST be registered LAST or it shadows
	// /quack, /health, /info, etc.

	// QuackHandlers: /quack, OPTIONS /quack
	quack_handlers = make_uniq<QuackHandlers>(*this, *sessions, *auth, db);
	quack_handlers->Register(*server);

	// AdminHandlers: /health, /info
	admin_handlers = make_uniq<AdminHandlers>(*this);
	admin_handlers->Register(*server);

	// UiHandlers: /localEvents (SSE), /localToken, /ddb/interrupt,
	// /ddb/run, /ddb/tokenize, GET /.* (catch-all proxy to ui.duckdb.org).
	// Registered LAST so the catch-all doesn't shadow earlier routes.
	// Construction also starts the catalog Watcher thread (stopped in
	// Shutdown() during Close()).
	ui_handlers = make_uniq<ui::UiHandlers>(*this, db, context);
	ui_handlers->Register(*server);
}

void FlockHttpServer::ListenThreadMain(FlockHttpServer *srv) {
	D_ASSERT(srv && srv->server);
	try {
		srv->server->listen_after_bind();
	} catch (...) {
		// Listener thread must not propagate exceptions; std::terminate
		// would abort the host process. The server's listening socket
		// will reflect that listening stopped; new requests will fail at
		// the socket layer.
		//
		// TODO PR-3+: route this through the Flock log type with the
		// exception string instead of swallowing silently. Today this
		// fails opaquely if listen_after_bind throws unexpectedly.
	}
}

void FlockHttpServer::StartListening() {
	std::lock_guard<std::mutex> lock(state_mu);
	if (state != State::BOUND) {
		throw InvalidInputException("FlockHttpServer::StartListening called in wrong state");
	}
	listen_thread = std::thread(ListenThreadMain, this);
	state = State::LISTENING;
}

void FlockHttpServer::StopAccepting() {
	// Idempotent. Only closes the listening socket — does NOT join the
	// listener thread, does NOT drain workers. Safe to call from a
	// request-handler thread.
	std::lock_guard<std::mutex> lock(state_mu);
	if (state == State::CLOSED || state == State::CLOSING) {
		return;
	}
	if (server) {
		server->stop();
	}
	state = State::CLOSING;
}

bool FlockHttpServer::DrainActiveRequests(std::chrono::seconds timeout) {
	std::unique_lock<std::mutex> lock(drain_mu);
	return cv_no_active.wait_for(lock, timeout, [this] {
		return active_requests.load(std::memory_order_acquire) == 0;
	});
}

void FlockHttpServer::ShutdownHandlers() {
	// Tell handlers to release non-request-scoped resources (threads,
	// blocking SSE waits, etc.) so the upcoming drain can actually
	// reach active_requests == 0. Without this, UiHandlers' Watcher
	// thread keeps polling and /localEvents WaitEvent blocks
	// forever — the drain never completes.
	//
	// Order matters: Watcher Stop() first (so no more events are
	// produced), then dispatcher Close() (so any in-flight WaitEvent
	// returns). UiHandlers::Shutdown() handles both internally.
	//
	// PR-3+ (per GPT-5.5 round 9 catch). PR-2 didn't need this because
	// quack handlers had no long-running threads — every action was
	// request-scoped and counted by ActiveRequestGuard.
	if (ui_handlers) {
		// Stops the Watcher thread + closes the EventDispatcher (wakes
		// any /localEvents WaitEvent calls). Without this, the drain
		// would block forever on the SSE consumers + the Watcher
		// holds a Connection that can keep the DB alive.
		ui_handlers->Shutdown();
	}
	if (admin_handlers) {
		// No long-running state today.
	}
	if (quack_handlers) {
		// No long-running state — every operation runs inside a
		// request handler counted by ActiveRequestGuard.
	}
}

void FlockHttpServer::Close() {
	StopAccepting();
	ShutdownHandlers();

	// Drain in-flight handlers. Per GPT-5.5 round 8 catch #2, we MUST
	// NOT destroy handlers while active_requests > 0 — route lambdas
	// capture `this` / handler pointers, and destroying under them
	// causes use-after-free.
	//
	// PR-2 policy: drain to zero, with a long polling loop. The
	// configured `flock_stop_drain_timeout_s` (default 30s) is the
	// per-attempt budget; if it expires while requests are still
	// in-flight we log a warning and try again. Forever-loop because
	// PR-2 has no interrupt mechanism — a hanging query genuinely
	// blocks shutdown until it completes. The PR-3+ interrupt path
	// (Connection::Interrupt() per session) will let us bound the
	// wait.
	const auto attempt_seconds = std::chrono::seconds(GetDrainTimeoutSec(db));
	while (!DrainActiveRequests(attempt_seconds)) {
		// Timeout expired; some request is still running. Re-check the
		// active count: if it's nonzero, keep waiting.
		auto remaining = active_requests.load(std::memory_order_acquire);
		if (remaining == 0) {
			break; // raced; we're done
		}
		// TODO PR-3+: invoke Connection::Interrupt() on each session and
		// then bound the wait. For now, log and loop.
	}

	std::lock_guard<std::mutex> lock(state_mu);
	if (listen_thread.joinable()) {
		listen_thread.join();
	}
	// Destroying handlers BEFORE the server itself ensures their lambda
	// captures (which borrow `this` and the subsystems) don't outlive
	// what they reference. We've drained to zero above, so no in-flight
	// callback should be touching them.
	ui_handlers.reset();
	admin_handlers.reset();
	quack_handlers.reset();
	server.reset();
	state = State::CLOSED;
}

SessionManager &FlockHttpServer::Sessions() {
	D_ASSERT(sessions);
	return *sessions;
}

AuthManager &FlockHttpServer::Auth() {
	D_ASSERT(auth);
	return *auth;
}

// -- FlockServerState (process-static singleton) --------------------------

FlockServerState &FlockServerState::Global() {
	// Function-static initialization is thread-safe in C++11 and later.
	static FlockServerState instance;
	return instance;
}

void FlockServerState::Start(ClientContext &context, weak_ptr<DatabaseInstance> db, QuackUri uri, string token) {
	std::lock_guard<std::mutex> lock(state_mu);
	if (server) {
		throw InvalidInputException("flock server is already running on %s — flock is single-server-per-process; "
		                            "call flock_stop or quack_stop first",
		                            server->ListenUri().Uri());
	}

	// Sequence: ctor → Bind() → RegisterBuiltinHandlers(context) → StartListening()
	auto srv = make_uniq<FlockHttpServer>(std::move(db), std::move(uri), std::move(token));
	srv->Bind();
	srv->RegisterBuiltinHandlers(context);
	srv->StartListening();

	server = std::move(srv);
	generation++;
	// Don't reset stopped_generation — Wait() checks `stopped_generation
	// >= my_generation`, so stale stopped_generation values from prior
	// runs harmlessly compare less-than the new generation.
}

bool FlockServerState::Stop(const QuackUri &requested_uri) {
	unique_ptr<FlockHttpServer> to_destroy;
	{
		std::lock_guard<std::mutex> lock(state_mu);
		if (!server) {
			return false;
		}
		// Only stop if the URI matches (by canonical form). This is mainly
		// defensive — single-server-per-process means there's at most one
		// server, but a stale flock_stop call with a different URI shouldn't
		// stop the wrong one.
		if (server->ListenUri().CanonicalUri() != requested_uri.CanonicalUri()) {
			return false;
		}
		to_destroy = std::move(server);
		stopped_generation = generation;
		cv.notify_all();
	}
	// Synchronous teardown. We previously did (StopAccepting + detached
	// destruction thread), but that allowed a transient "two-servers"
	// state where flock_serve could rebind the freed port while the
	// detached thread was still draining the old server's handlers
	// (per GPT-5.5 round 8 catch #3). Synchronous Close blocks the SQL
	// caller for the duration of in-flight requests but eliminates the
	// race. Practically, the drain finishes in milliseconds for normal
	// workloads — only a hanging query could make this slow, and the
	// PR-3 interrupt path will address that case.
	to_destroy->Close();
	return true;
}

bool FlockServerState::Wait() {
	std::unique_lock<std::mutex> lock(state_mu);
	if (!server) {
		throw InvalidInputException("flock_wait called with no server running — call flock_serve first");
	}
	auto my_generation = generation;
	cv.wait(lock, [this, my_generation] {
		return stopped_generation >= my_generation;
	});
	return true;
}

bool FlockServerState::IsRunning() {
	std::lock_guard<std::mutex> lock(state_mu);
	return server != nullptr;
}

void FlockServerState::WithCurrent(const std::function<void(FlockHttpServer &)> &fn) {
	std::lock_guard<std::mutex> lock(state_mu);
	if (server) {
		fn(*server);
	}
}

} // namespace duckdb
