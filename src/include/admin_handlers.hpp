#pragma once

// AdminHandlers — admin and operational HTTP routes.
//
// Pre-PR-6 this owned just /health and /info (the two public probes
// the DuckDB UI relies on for server detection). PR-6 extended the
// surface to the full SPEC §4 admin route table:
//
//   GET  /health                     public  (PR-2)
//   GET  /info                       public  (PR-2)
//   GET  /ready                      public  (PR-6) — runs SELECT 1; 503 on failure
//   GET  /whoami                     bearer/cookie + authz __HARBOR_ADMIN__:server:whoami
//   GET  /tables                     bearer/cookie + authz __HARBOR_ADMIN__:catalog:list_tables
//   GET  /schema/:db/:table          bearer/cookie + authz __HARBOR_ADMIN__:catalog:describe_table
//   POST /checkpoint                 bearer/cookie + authz __HARBOR_ADMIN__:checkpoint:create
//   GET  /sessions                   bearer/cookie + authz __HARBOR_ADMIN__:sessions:list
//   POST /interrupt                  bearer/cookie + authz __HARBOR_ADMIN__:sessions:interrupt
//
// Path parameters on /schema/:db/:table are NEVER concatenated into
// the authz string (per SPEC §7) — the policy decision input is the
// stable resource:action pair, the concrete identifiers go through
// the request envelope only. They are also never string-interpolated
// into SQL: /schema uses pragma_show_columns on a quoted identifier
// path through the system view duckdb_columns().
//
// The internal default-deny rule on __HARBOR_ADMIN__:* enforced by
// AuthManager::RunAuthorization (PR-6) means these routes are
// inaccessible until the operator either configures a custom
// harbor_authorization_function or sets harbor_allow_admin_without_authz=true.
//
// All routes are wrapped in HarborHttpServer::ActiveRequestGuard so
// they participate in the drain-on-close handshake.

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include "duckdb/common/common.hpp"

namespace duckdb {

class HarborHttpServer;
class AuthManager;
class SessionManager;
class DatabaseInstance;

class AdminHandlers {
public:
	// PR-6 — synthetic admin authz strings (per SPEC §7 routes table).
	// Centralized here so SqlHandlers can also reach them when
	// /sql/cancel needs the matching string. Never user-supplied.
	static constexpr const char *kAuthzWhoami           = "__HARBOR_ADMIN__:server:whoami";
	static constexpr const char *kAuthzListTables       = "__HARBOR_ADMIN__:catalog:list_tables";
	static constexpr const char *kAuthzDescribeTable    = "__HARBOR_ADMIN__:catalog:describe_table";
	static constexpr const char *kAuthzCheckpointCreate = "__HARBOR_ADMIN__:checkpoint:create";
	static constexpr const char *kAuthzSessionsList     = "__HARBOR_ADMIN__:sessions:list";
	static constexpr const char *kAuthzSessionsInterrupt = "__HARBOR_ADMIN__:sessions:interrupt";

	// Synthetic session id for AuthManager::AuthenticateRequest +
	// RunAuthorization on admin routes. Mirrors SqlHandlers' kAdminSessionId.
	static constexpr const char *kAdminSessionId = "__HARBOR_AUTH__:admin";

	AdminHandlers(HarborHttpServer &server, AuthManager &auth, SessionManager &sessions,
	              weak_ptr<DatabaseInstance> db);
	~AdminHandlers();

	AdminHandlers(const AdminHandlers &) = delete;
	AdminHandlers &operator=(const AdminHandlers &) = delete;

	// Register all admin routes against the shared server. Must be
	// called between HarborHttpServer::Bind() and StartListening().
	void Register(duckdb_httplib_openssl::Server &server);

private:
	// Public probe routes (no auth). Same shape as PR-2.
	void HandleHealth(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleInfo(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleReady(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);

	// Authn-required routes. Each first calls AuthenticateRequest +
	// RunAuthorization with the matching kAuthz* synthetic string, then
	// dispatches if both pass. Mutating POSTs additionally check
	// Origin/Referer (CSRF) when the credential source is the harbor
	// session cookie, and require Content-Type: application/json with
	// a body capped at harbor_max_request_body_bytes.
	void HandleWhoami(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleTables(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleSchema(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleCheckpoint(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res,
	                      const duckdb_httplib_openssl::ContentReader &content_reader);
	void HandleSessions(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleInterrupt(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res,
	                     const duckdb_httplib_openssl::ContentReader &content_reader);

	// Borrowed references; lifetimes managed by HarborHttpServer.
	HarborHttpServer &server;
	AuthManager &auth;
	SessionManager &sessions;
	weak_ptr<DatabaseInstance> db;
};

} // namespace duckdb
