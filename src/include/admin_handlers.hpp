#pragma once

// AdminHandlers — minimal /health and /info routes for PR-2.
//
// The full admin surface (/whoami, /tables, /schema, /checkpoint,
// /sessions, /interrupt) per SPEC §4 lands in PR-5 with the
// __FLOCK_ADMIN__:resource:action authz integration.
//
// PR-2 just exposes:
//   GET /health  → public; {ok: true, version, uptime_s}
//   GET /info    → public; empty body, version headers (used by the
//                  DuckDB UI to detect the server)
//
// Both routes are wrapped in FlockHttpServer::ActiveRequestGuard so
// they participate in the drain-on-close handshake.

// Match FlockHttpServer's openssl-enabled cpp-httplib (see header
// comment in flock_http_server.hpp explaining the namespace migration).
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

namespace duckdb {

class FlockHttpServer;

class AdminHandlers {
public:
	explicit AdminHandlers(FlockHttpServer &server);

	// Register /health and /info against the shared server. Must be
	// called between FlockHttpServer::Bind() and StartListening().
	void Register(duckdb_httplib_openssl::Server &server);

private:
	FlockHttpServer &server;
};

} // namespace duckdb
