#pragma once

// AuthHandlers — registers authentication-related HTTP routes against
// the shared HarborHttpServer:
//
//   POST   /auth/login     — exchange bearer/X-Harbor-Token for a
//                            harbor_session cookie
//   POST   /auth/logout    — clear the cookie (always 200, no info leak)
//   OPTIONS /auth/login    — CORS preflight (when allow-list configured)
//   OPTIONS /auth/logout   — CORS preflight
//   OPTIONS /quack         — CORS preflight (admin/SQL routes get
//                            their preflights in PR-5/PR-6)
//
// AuthHandlers is the natural home for the cookie-issuance flow because:
//   - It needs AuthManager (RunAuthentication + IssueCookie)
//   - It needs to detect X-Forwarded-Proto for the Secure cookie flag
//   - It manages CORS preflight responses for routes whose request
//     handlers live elsewhere (the preflight policy is uniform; the
//     actual mutating-request handler is in QuackHandlers / future
//     SqlHandlers)
//
// The harbor-specific GET / login wrapper lives inside UiHandlers'
// catch-all (Option B per round-11 review): a single code path owns
// "serve UI asset", with the cookie check inline. Two separate route
// registrations would force handler-recursion when an authenticated
// user requests / and we want to proxy upstream.

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include "duckdb/common/common.hpp"

namespace duckdb {

class HarborHttpServer;
class AuthManager;
class SessionManager;

class AuthHandlers {
public:
	// Synthetic session id passed to AuthManager::RunAuthentication
	// when /auth/login authenticates the bearer token. Same shape as
	// the __HARBOR_ADMIN__:resource:action convention so authz macros
	// can pattern-match consistently.
	static constexpr const char *kLoginSessionId = "__HARBOR_AUTH__:login";

	// Cookie name. SPEC §7. Don't change without a migration plan.
	static constexpr const char *kCookieName = "harbor_session";

	// Default cookie TTL (12 hours). Overridden by the
	// harbor_auth_cookie_ttl_s setting if configured.
	static constexpr uint64_t kDefaultCookieTtlSec = 43200;

	AuthHandlers(HarborHttpServer &server, AuthManager &auth, SessionManager &sessions);
	~AuthHandlers();

	AuthHandlers(const AuthHandlers &) = delete;
	AuthHandlers &operator=(const AuthHandlers &) = delete;

	void Register(duckdb_httplib_openssl::Server &http);

private:
	void HandleLogin(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);
	void HandleLogout(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);

	// Wire CORS preflight on a route. No-op (returns 204 with empty
	// headers) when the request Origin isn't in the allow-list, or
	// when the allow-list is empty.
	void HandlePreflight(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res);

	// Read harbor_auth_cookie_ttl_s from settings, falling back to the
	// 12h default. Cap at uint32_t-max-seconds to keep Set-Cookie
	// Max-Age sane (browsers reject huge Max-Age).
	uint64_t CookieTtlSec();

	// Detect whether the response cookie should set the Secure flag.
	// We never directly TLS-terminate, so we rely on the reverse
	// proxy passing X-Forwarded-Proto=https. NEVER use
	// X-Forwarded-Proto for authorization decisions — only for the
	// cookie attribute (per round-11 review).
	bool RequestIsBehindHttps(const duckdb_httplib_openssl::Request &req);

	HarborHttpServer &server;
	AuthManager &auth;
	SessionManager &sessions;
};

} // namespace duckdb
