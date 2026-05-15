#pragma once

// SqlHandlers — POST /sql, POST /sql/sessions/new, DELETE /sql/sessions/:id.
//
// The /sql endpoint per SPEC §5.2-5.4: app-friendly JSON SQL with
// NDJSON streaming default and one-shot JSON mode for tiny queries.
//
// Routes registered (in this order; CORS preflight OPTIONS /sql lives
// in AuthHandlers alongside /quack and /auth/* preflights):
//
//   POST   /sql                          (main entry; auth + authz + execute)
//   POST   /sql/sessions/new             (create explicit session for transactions)
//   DELETE /sql/sessions/:id             (destroy explicit session)
//
// /sql/cancel is intentionally NOT in PR-5 (admin-authz; ships with PR-6).

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include "duckdb/common/common.hpp"

namespace duckdb {

class FlockHttpServer;
class AuthManager;
class SessionManager;
class DatabaseInstance;

class SqlHandlers {
public:
	// Synthetic session id used when calling AuthManager::AuthenticateRequest
	// and RunAuthorization on requests that don't have a real DB
	// session (e.g. /sql/sessions/new before a session exists).
	static constexpr const char *kAdminSessionId = "__FLOCK_AUTH__:sql";
	// Synthetic admin SQL injected into authz callback for session-create.
	// Per SPEC §7 admin authz table: __FLOCK_ADMIN__:sessions:create.
	static constexpr const char *kAuthzCreateSession = "__FLOCK_ADMIN__:sessions:create";
	static constexpr const char *kAuthzDeleteSession = "__FLOCK_ADMIN__:sessions:delete";

	SqlHandlers(FlockHttpServer &server, AuthManager &auth, SessionManager &sessions, weak_ptr<DatabaseInstance> db);
	~SqlHandlers();

	SqlHandlers(const SqlHandlers &) = delete;
	SqlHandlers &operator=(const SqlHandlers &) = delete;

	void Register(duckdb_httplib_openssl::Server &http);

private:
	// Main /sql handler. Dispatches to one of the streaming/one-shot
	// paths based on Accept header.
	void HandleSql(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res,
	               const duckdb_httplib_openssl::ContentReader &content_reader);

	// Session-lifecycle handlers.
	void HandleSessionNew(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleSessionDelete(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);

	// Borrowed references; lifetimes managed by FlockHttpServer.
	FlockHttpServer &server;
	AuthManager &auth;
	SessionManager &sessions;
	weak_ptr<DatabaseInstance> db;
};

} // namespace duckdb
