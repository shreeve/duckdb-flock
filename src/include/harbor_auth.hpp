#pragma once

// AuthManager — server-token + auth/authz callback resolution +
// (PR-4) cookie issuance/verification and CORS allow-list resolution.
//
// PR-2 lifted the auth-related members and EvaluateAuthQuery free
// function from upstream QuackServer.
//
// PR-4 layered on:
//   - HMAC-signed harbor_session cookie: IssueCookie + VerifyCookie.
//     Signing key is process-static, ephemeral random (32 bytes from
//     RAND_bytes), lazy-initialized on first use under a mutex. There
//     is deliberately NO SQL setting for the signing key — exposing
//     the HMAC secret to authorized SQL would let any SQL caller mint
//     cookies. v0.2 will reintroduce operator control via the
//     HARBOR_COOKIE_SIGNING_KEY environment variable. See SPEC §7 +
//     §15 question 2.
//   - AuthenticateRequest: unified entry point for cookie/bearer/
//     X-Harbor-Token detection with explicit precedence (Bearer >
//     X-Harbor-Token > Cookie). Bad bearer NEVER falls back to cookie
//     (prevents ambient browser state from masking explicit-credential
//     failures).
//   - CORS allow-list (harbor_cors_origins setting): InitCorsConfig
//     reads the setting at harbor_serve time and refuses to start if
//     it's '*'. ResolveCorsOrigin returns the matching origin (or
//     empty) for a given request Origin header.
//
// Cryptographic primitives (SHA-256, HMAC-SHA256, RAND_bytes,
// base64url, constant-time compare) live in src/harbor_crypto.{cpp,hpp}.
// AuthManager calls them; it doesn't reimplement them.

#include <cstdint>
#include <mutex>
#include <vector>

#include "duckdb/common/common.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"

// Forward-declare instead of including httplib.hpp here — the
// AuthenticateRequest signature uses `const httplib::Request &`, but
// pulling httplib into every translation unit that includes
// harbor_auth.hpp would be a heavy dependency. The .cpp does the include.
namespace duckdb_httplib_openssl {
struct Request;
}

namespace duckdb {

class DatabaseInstance;

// Where the credential came from on a given request. Used by handlers
// for log-line attribution and (future) per-source policy choices.
enum class AuthSource : uint8_t {
	kNone = 0,
	kBearer,        // Authorization: Bearer <token>
	kXHarborToken,   // X-Harbor-Token: <token>
	kCookie,        // Cookie: harbor_session=<signed>
	kLocalDev       // harbor_local_dev_mode bypass; principal is "__local_dev__"
};

struct AuthResult {
	bool ok = false;
	string principal_id;          // hex(sha256(token)); empty when ok=false
	AuthSource source = AuthSource::kNone;
	// Stable error code suitable for HTTP body: "MISSING_CREDENTIAL",
	// "INVALID_TOKEN", "BAD_COOKIE_FORMAT", "BAD_COOKIE_SIG",
	// "COOKIE_EXPIRED". Empty on ok.
	string error_code;
};

// Result of CORS allow-list resolution. `origin` is the exact value
// to echo back in Access-Control-Allow-Origin (never '*'). `allowed`
// is true iff the request origin matched.
struct CorsDecision {
	bool allowed = false;
	string origin;
};

class AuthManager {
public:
	// `unauthenticated` (v0.2): when true, the auth gate is open —
	// AuthenticateRequest returns success immediately with the
	// synthetic `harbor.local-dev` principal, and RunAuthentication
	// returns true unconditionally. All presented credentials
	// (Bearer / Cookie / X-Harbor-Token / Quack AuthString) are
	// ignored. Triggered exclusively by `harbor_serve(uri, token := NULL)`
	// on a loopback bind. Snapshotted at server-start; immutable for the
	// lifetime of the running server.
	AuthManager(weak_ptr<DatabaseInstance> db, string server_token, bool unauthenticated);
	~AuthManager();

	// Public, stable principal id assigned to every request when harbor
	// is running in unauthenticated mode. Human-readable in audit logs;
	// has no colon (avoids collision with `__HARBOR_ADMIN__:resource:action`
	// authz format).
	static const string &LocalDevPrincipalId();

	// True iff the operator configured a non-default
	// harbor_authorization_function at server start. Snapshotted at
	// AuthManager construction; immutable for the running server's
	// lifetime. Use this from request handlers and from the start-time
	// "loud WARN log" path; do NOT call IsAdminAuthzCustomConfigured(db)
	// per request (that path reads live settings — TOCTOU window).
	bool HasCustomAuthzConfigured() const {
		return snapshot_has_custom_authz_fn;
	}

	AuthManager(const AuthManager &) = delete;
	AuthManager &operator=(const AuthManager &) = delete;

	const string &ServerToken() const {
		return server_token;
	}

	// Throws InvalidInputException if token is shorter than 4 chars.
	// Same contract as upstream QuackServer::ValidateToken.
	static void ValidateToken(const string &token);

	// PR-6 follow-up (round 19): single source of truth for the
	// "custom authz configured?" rule. Returns true iff at least one
	// of harbor_authorization_function / quack_authorization_function
	// is set to something other than empty AND something other than
	// the built-in NOP fn names (`harbor_nop_authorization`,
	// `quack_nop_authorization`). Used by RunAuthorization's centralized
	// default-deny on __HARBOR_ADMIN__:* AND by harbor_serve's loud
	// startup WARN when admin-bypass is in effect.
	static bool IsAdminAuthzCustomConfigured(DatabaseInstance &db);

	// CSPRNG-backed 16-byte (128-bit) token, hex-encoded (32 chars).
	// Delegates to db.GetEncryptionUtil(), which in DuckDB's default
	// configuration auto-loads the httpfs extension and uses its
	// OpenSSL-backed EncryptionUtil — so this IS transitively
	// OpenSSL's RAND_bytes; we just don't link OpenSSL ourselves for
	// THIS call (PR-4's harbor_crypto.cpp does link and use OpenSSL
	// directly for cookie signing key + nonces, with no httpfs
	// coupling). The only non-OpenSSL paths inside DuckDB's encryption
	// dispatch are deliberately insecure (force_mbedtls_unsafe='true'
	// routes through mbedTLS's Mersenne-Twister fallback — explicitly
	// NOT a CSPRNG) or fail outright (read-only mode throws
	// InvalidConfigurationException). So "uses GetEncryptionUtil"
	// practically means "uses OpenSSL via httpfs."
	static string GenerateRandomToken(DatabaseInstance &db);

	// Run the authentication callback against a transient Connection.
	// Reads callback settings in Harbor-primary / Quack-compatible
	// order: `harbor_authentication_function`, then
	// `quack_authentication_function`, then fallback
	// `harbor_check_token`. Invokes the selected callback as
	// `SELECT <fn_name>(?, ?, ?)` with `(session_id, client_token,
	// server_token)` bound as prepared parameters — never
	// string-formatted. Returns false on any failure path (NULL,
	// non-bool, exception, false).
	bool RunAuthentication(const string &session_id, const string &client_token);

	// Run the authorization callback against a transient Connection.
	// Resolution order: `harbor_authorization_function`, then
	// `quack_authorization_function`, then fallback
	// `harbor_nop_authorization`. See harbor_auth.cpp for SQL shape
	// and failure-path contract.
	bool RunAuthorization(const string &session_id, const string &query);

	// ---- PR-4: cookie issuance + verification ----

	// Issue a signed harbor_session cookie value for `principal_hex`
	// expiring `ttl_s` seconds from now. Format (per SPEC §7):
	//
	//   v1 . b64url(principal_hex)
	//      . b64url(expires_unix_ascii)
	//      . b64url(nonce16)
	//      . b64url(hmac32)
	//
	// HMAC is over the exact ASCII bytes "v1.<seg1>.<seg2>.<seg3>"
	// using the process-static signing key (lazy-initialized on first
	// call). Returns the cookie value (NOT including the
	// "harbor_session=" name or attributes — that's the handler's job).
	string IssueCookie(const string &principal_hex, uint64_t ttl_s);

	// Verify a harbor_session cookie value. Returns ok=true iff:
	//   - the value parses as v1.<seg1>.<seg2>.<seg3>.<seg4>
	//   - HMAC over "v1.<seg1>.<seg2>.<seg3>" matches seg4 (constant-time)
	//   - principal_hex (decoded seg1) is 64 lowercase hex chars
	//   - expires_unix (decoded seg2) is in the future relative to now
	// On failure, error_code is set; principal_id stays empty.
	AuthResult VerifyCookie(const string &cookie_value);

	// Unified per-request authentication. Parses Authorization,
	// X-Harbor-Token, and Cookie headers per precedence:
	//
	//   1. Authorization: Bearer <token>     — runs RunAuthentication;
	//                                           on FAIL returns 401
	//                                           (no fallback to cookie)
	//   2. X-Harbor-Token: <token>            — same as bearer
	//   3. Cookie: harbor_session=<value>     — VerifyCookie (HMAC only)
	//
	// `synthetic_session_id` is passed to RunAuthentication when
	// either of the explicit headers is present; common values are
	// "__HARBOR_AUTH__:login" (for /auth/login) or the per-route sid
	// chosen by the calling handler.
	AuthResult AuthenticateRequest(const duckdb_httplib_openssl::Request &req,
	                               const string &synthetic_session_id);

	// ---- PR-4: CORS allow-list ----

	// Parse `harbor_cors_origins` setting at harbor_serve time. Throws
	// InvalidInputException if the setting is "*" (wildcard +
	// credentials is forbidden by spec and by us — see SPEC §7).
	// Tolerates empty / whitespace-only entries (skipped). Each entry
	// must be a well-formed origin: scheme://host[:port], no path,
	// no query, no fragment, no trailing slash.
	void InitCorsConfig(const string &cors_origins_setting);

	// Resolve an incoming Origin header against the configured
	// allow-list. Returns CorsDecision{allowed=true, origin=<exact
	// match>} or CorsDecision{allowed=false}. The `origin` field is
	// always the exact value to echo back (never '*').
	CorsDecision ResolveCorsOrigin(const string &request_origin) const;

	// True iff any CORS origins are configured. Handlers can short-
	// circuit OPTIONS preflight setup when no allow-list exists.
	bool CorsConfigured() const {
		return !cors_allowed_origins.empty();
	}

private:
	// Lazy-initialize the cookie signing key under mutex. Returns a
	// const reference into the member vector; the vector is never
	// resized after init, so the reference is stable.
	const std::vector<uint8_t> &CookieSigningKey();

	// Validate a single CORS origin string. Throws on malformed input.
	static void ValidateCorsOrigin(const string &origin);

	weak_ptr<DatabaseInstance> db;
	string server_token;
	bool unauthenticated;

	// v0.2 Stage 7 (snapshot at server start, per GPT-5.5 round-26
	// blocker): the authn/authz function names are resolved ONCE at
	// AuthManager construction time and stored on the instance. Request
	// handlers MUST use these snapshots — they MUST NOT re-read
	// `harbor_authentication_function` / `harbor_authorization_function`
	// per request. This closes the TOCTOU window where an authenticated
	// SQL caller could `SET GLOBAL harbor_authentication_function = 'allow_all'`
	// mid-process and silently broaden auth for everyone else.
	string snapshot_authn_fn_name;
	string snapshot_authz_fn_name;
	bool snapshot_has_custom_authn_fn = false;
	bool snapshot_has_custom_authz_fn = false;

	std::mutex signing_key_mutex;
	std::vector<uint8_t> signing_key; // 32 random bytes; init on first IssueCookie/VerifyCookie call

	// Parsed harbor_cors_origins. Each entry is an exact Origin header
	// value to match against (e.g. "https://app.example.com",
	// "http://localhost:3000"). Comparison is byte-equal.
	std::vector<string> cors_allowed_origins;
};

} // namespace duckdb
