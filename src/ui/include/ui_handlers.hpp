#pragma once

// UiHandlers — registers DuckDB UI routes (/info, /localEvents,
// /localToken, /ddb/*, GET /.* proxy) against the shared
// FlockHttpServer. Replaces upstream duckdb-ui's HttpServer class:
//
//   - Drops the singleton pattern (server_instance, atexit,
//     Started/Start/Stop/IsRunningOnMachine, GetInstance).
//   - Drops the embedded duckdb_httplib_openssl::Server and listener
//     thread (those live on FlockHttpServer now, per PR-2's
//     architecture).
//   - Keeps the route handler bodies (HandleGetInfo et al.) verbatim
//     where reasonable.
//   - Owns the long-running EventDispatcher + Watcher that push
//     "catalog changed" events to /localEvents SSE clients.
//
// Lifecycle (called by FlockHttpServer::RegisterBuiltinHandlers and
// FlockHttpServer::Close):
//
//   ui_handlers = make_uniq<UiHandlers>(*this, db, context);
//   ui_handlers->Register(server);   // registers routes + starts Watcher
//   ...
//   ui_handlers->Shutdown();         // stops Watcher + closes EventDispatcher
//                                    // (called from FlockHttpServer::Close()
//                                    // BEFORE the active-request drain — the
//                                    // Watcher thread isn't request-scoped)
//
// Auth model (PR-3):
//   - /ddb/run, /ddb/interrupt, /ddb/tokenize: Origin must match an
//     entry in our local-allowed set ({localhost, 127.0.0.1, [::1]}
//     plus the bind host if not 0.0.0.0).
//   - /localToken: Referer must start with our local URL AND the
//     bind host must be loopback (per SPEC §7).
//   - /info, /localEvents, GET /.*: public (no auth).
//
// PR-4 will replace the Origin-set check with the cookie-auth flow
// (HMAC-signed flock_session cookie + Origin == flock_cors_origins).

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "duckdb/common/common.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"

// Match FlockHttpServer's openssl-enabled cpp-httplib (see header
// comment in src/include/flock_http_server.hpp explaining the namespace
// migration).
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

namespace duckdb {

class DatabaseInstance;
class ClientContext;
class FlockHttpServer;
struct HTTPParams;
class MemoryStream;

namespace ui {

class EventDispatcher;
class Watcher;

class UiHandlers {
public:
	UiHandlers(FlockHttpServer &server, weak_ptr<DatabaseInstance> db, ClientContext &context);
	~UiHandlers();

	UiHandlers(const UiHandlers &) = delete;
	UiHandlers &operator=(const UiHandlers &) = delete;

	// Register routes against the shared httplib server. Must be
	// called between FlockHttpServer::Bind() and StartListening().
	// Also starts the Watcher (which polls the DuckDB catalog and
	// pushes "catalog changed" events to /localEvents SSE clients).
	//
	// Routes registered (in this order):
	//   GET  /localEvents       (SSE)
	//   GET  /localToken        (Referer-checked, loopback-only)
	//   POST /ddb/interrupt     (Origin-checked)
	//   POST /ddb/run           (Origin-checked)
	//   POST /ddb/tokenize      (Origin-checked)
	//   GET  /.*                (catch-all proxy to ui.duckdb.org)
	//
	// /info is NOT registered here — that's owned by AdminHandlers,
	// which extends its response to include X-DuckDB-UI-Extension-Version
	// so UI clients can detect the server.
	//
	// The catch-all GET /.* must be the LAST route registered against
	// the server (cpp-httplib resolves in registration order). Caller
	// (FlockHttpServer::RegisterBuiltinHandlers) is responsible for
	// calling UiHandlers::Register() AFTER all other handler subsystems'
	// Register() calls.
	void Register(duckdb_httplib_openssl::Server &server);

	// Stop the Watcher thread and close the EventDispatcher (which
	// wakes any /localEvents SSE consumers blocked in WaitEvent).
	// Called by FlockHttpServer::Close() before draining active
	// requests. Idempotent.
	void Shutdown();

	// The version string reported in /info's X-DuckDB-UI-Extension-Version
	// header. AdminHandlers::Register reads this so its /info response
	// matches what upstream UI clients expect.
	static const char *UiExtensionVersion();

private:
	// ---- Per-request handlers (mirror upstream HttpServer's Handle* methods) ----
	void HandleGetLocalEvents(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleGetLocalToken(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleInterrupt(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleRun(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res,
	               const duckdb_httplib_openssl::ContentReader &content_reader);
	void DoHandleRun(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res,
	                 const duckdb_httplib_openssl::ContentReader &content_reader);
	void HandleTokenize(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res,
	                    const duckdb_httplib_openssl::ContentReader &content_reader);
	void HandleProxyGet(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);

	// ---- Helpers ----
	std::string ReadContent(const duckdb_httplib_openssl::ContentReader &content_reader);
	void SetResponseContent(duckdb_httplib_openssl::Response &res, const MemoryStream &content);
	void SetResponseEmptyResult(duckdb_httplib_openssl::Response &res);
	void SetResponseErrorResult(duckdb_httplib_openssl::Response &res, const std::string &error);
	shared_ptr<DatabaseInstance> LockDatabaseInstance();
	void InitClientFromParams(duckdb_httplib_openssl::Client &client);

	// True iff `origin` is one of the configured local-allowed
	// origins (set by ComputeAllowedOrigins() at construction time).
	bool IsAllowedOrigin(const std::string &origin) const;

	// True iff the bind host is loopback (localhost, 127.0.0.1, ::1).
	// /localToken returns 404 unless this is true.
	bool IsBoundLocally() const;

	// Computed at construction; the set of Origin header values that
	// pass the /ddb/* same-origin check. Includes `http://localhost:N`,
	// `http://127.0.0.1:N`, `http://[::1]:N`, plus `http://<bind_host>:N`
	// if bind_host is concrete (not 0.0.0.0).
	std::vector<std::string> ComputeAllowedOrigins() const;

	// Compute the "local URL prefix" for the /localToken Referer check.
	// `http://localhost:N/` (trailing slash matters per upstream).
	std::string ComputeLocalUrlPrefix() const;

	// ---- Members ----
	FlockHttpServer &server;
	weak_ptr<DatabaseInstance> ddb_instance;

	std::string remote_url;             // ui_remote_url setting at construction
	std::vector<std::string> allowed_origins; // for /ddb/* Origin checks
	std::string local_url_prefix;       // for /localToken Referer check
	std::string user_agent;             // for outbound proxy requests
	uint32_t polling_interval_ms;       // for Watcher

	unique_ptr<HTTPParams> http_params; // for outbound proxy client config
	unique_ptr<EventDispatcher> event_dispatcher;
	unique_ptr<Watcher> watcher;        // started by Register(), stopped by Shutdown()

	bool shutdown_called = false;       // for Shutdown() idempotency
};

} // namespace ui
} // namespace duckdb
