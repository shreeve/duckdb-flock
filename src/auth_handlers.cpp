#include "auth_handlers.hpp"

#include "flock_auth.hpp"
#include "flock_crypto.hpp"
#include "flock_http_server.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace duckdb {

namespace {

// Largest body we accept on /auth/login. The body is JSON like
// {"token":"<32-hex-chars>"} so 4 KiB is comfortably enough; anything
// bigger is either a misuse or a payload-bomb probe.
constexpr size_t kMaxLoginBodyBytes = 4096;

// Headers we allow on cross-origin preflighted requests. Exposed in
// Access-Control-Allow-Headers when CORS is enabled and the request
// Origin matches the allow-list. Per SPEC §7.
constexpr const char *kAllowedCorsHeaders =
    "Authorization, Content-Type, X-Flock-Token, X-Flock-Session-Id, Accept";

// Cap on Set-Cookie Max-Age. ~10 years; far above the 12h default but
// finite so a misconfigured setting can't produce a session-effectively-
// permanent cookie.
constexpr uint64_t kCookieMaxAgeCap = 315576000ULL;

// In-place trim helpers — DuckDB's StringUtil::Trim mutates in place
// and returns void, which doesn't compose with `token = trim(...)`.
// We use a private overload that returns a copy.
string TrimCopy(const string &s) {
	string out = s;
	StringUtil::Trim(out);
	return out;
}

string EscapeJsonString(const string &s) {
	string out;
	out.reserve(s.size() + 2);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
		}
	}
	return out;
}

// Parse `{"token":"<value>"}` minimally — we control the producer
// (the login page) so don't pull in a JSON dependency. Returns the
// extracted token, or empty string if malformed.
//
// Recognized shapes:
//   {"token":"abc"}
//   { "token" : "abc" }
//   {"token": "abc"}
// Everything else (escaped chars in the value, multi-key objects,
// etc.) is rejected — we deliberately don't try to parse arbitrary
// JSON. Callers should prefer the Authorization header anyway.
string ExtractTokenFromBody(const string &body) {
	auto key_pos = body.find("\"token\"");
	if (key_pos == string::npos) {
		return string();
	}
	auto colon = body.find(':', key_pos + 7);
	if (colon == string::npos) {
		return string();
	}
	auto first_quote = body.find('"', colon + 1);
	if (first_quote == string::npos) {
		return string();
	}
	auto second_quote = body.find('"', first_quote + 1);
	if (second_quote == string::npos) {
		return string();
	}
	auto value = body.substr(first_quote + 1, second_quote - first_quote - 1);
	// Reject anything with an escape — keep semantics simple.
	if (value.find('\\') != string::npos) {
		return string();
	}
	return value;
}

string IsoUtcFromUnix(uint64_t unix_seconds) {
	std::time_t t = static_cast<std::time_t>(unix_seconds);
	std::tm tm_buf {};
#if defined(_WIN32)
	gmtime_s(&tm_buf, &t);
#else
	gmtime_r(&t, &tm_buf);
#endif
	std::ostringstream oss;
	oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
	return oss.str();
}

void WriteJsonError(duckdb_httplib_openssl::Response &res, int status, const string &error_code,
                    const string &message) {
	res.status = status;
	auto body = StringUtil::Format("{\"error\":\"%s\",\"message\":\"%s\"}",
	                               EscapeJsonString(error_code), EscapeJsonString(message));
	res.set_content(body, "application/json");
}

// Apply CORS-allow-origin headers to a response IF the request has an
// Origin header AND that origin is in the allow-list. No-op otherwise.
// Uses ResolveCorsOrigin so the echoed origin is byte-equal to the
// allow-list entry (never the request's Origin verbatim, which could
// differ in case or trailing slash).
void ApplyCorsHeaders(AuthManager &auth, const duckdb_httplib_openssl::Request &req,
                      duckdb_httplib_openssl::Response &res) {
	auto origin = req.get_header_value("Origin");
	if (origin.empty()) {
		return;
	}
	auto decision = auth.ResolveCorsOrigin(origin);
	if (!decision.allowed) {
		return;
	}
	res.set_header("Access-Control-Allow-Origin", decision.origin);
	res.set_header("Access-Control-Allow-Credentials", "true");
	res.set_header("Vary", "Origin");
}

} // namespace

AuthHandlers::AuthHandlers(FlockHttpServer &server_p, AuthManager &auth_p)
    : server(server_p), auth(auth_p) {
}

AuthHandlers::~AuthHandlers() {
}

uint64_t AuthHandlers::CookieTtlSec() {
	auto db_locked = server.Database().lock();
	if (!db_locked) {
		return kDefaultCookieTtlSec;
	}
	Value setting_val;
	auto &config = DBConfig::GetConfig(*db_locked);
	if (!config.TryGetCurrentSetting("flock_auth_cookie_ttl_s", setting_val) || setting_val.IsNull()) {
		return kDefaultCookieTtlSec;
	}
	try {
		auto v = setting_val.GetValue<uint64_t>();
		if (v == 0) {
			return kDefaultCookieTtlSec;
		}
		if (v > kCookieMaxAgeCap) {
			return kCookieMaxAgeCap;
		}
		return v;
	} catch (...) {
		return kDefaultCookieTtlSec;
	}
}

bool AuthHandlers::RequestIsBehindHttps(const duckdb_httplib_openssl::Request &req) {
	auto xfp = req.get_header_value("X-Forwarded-Proto");
	if (xfp.empty()) {
		return false;
	}
	return StringUtil::Lower(xfp) == "https";
}

void AuthHandlers::HandleLogin(const duckdb_httplib_openssl::Request &req,
                               duckdb_httplib_openssl::Response &res) {
	FlockHttpServer::ActiveRequestGuard guard(server);
	ApplyCorsHeaders(auth, req, res);

	// Token resolution precedence on /auth/login (different from the
	// standard request-auth precedence — for /auth/login itself, the
	// JSON body is the canonical form because the login page POSTs
	// the user-pasted token there):
	//
	//   1. Authorization: Bearer <token>
	//   2. X-Flock-Token: <token>
	//   3. JSON body {"token":"<token>"}
	//
	// We accept all three so curl users, browser users, and
	// programmatic clients can all reach this endpoint naturally.
	string token;
	auto auth_header = req.get_header_value("Authorization");
	auto x_flock = req.get_header_value("X-Flock-Token");
	if (!auth_header.empty() && auth_header.size() > 7 &&
	    auth_header.compare(0, 7, "Bearer ") == 0) {
		token = TrimCopy(auth_header.substr(7));
	} else if (!x_flock.empty()) {
		token = TrimCopy(x_flock);
	} else if (!req.body.empty()) {
		if (req.body.size() > kMaxLoginBodyBytes) {
			WriteJsonError(res, 413, "BODY_TOO_LARGE",
			               "request body exceeds the /auth/login limit");
			return;
		}
		token = ExtractTokenFromBody(req.body);
	}

	if (token.empty()) {
		WriteJsonError(res, 400, "MISSING_CREDENTIAL",
		               "/auth/login requires Authorization, X-Flock-Token, or {\"token\":\"...\"} body");
		return;
	}

	if (!auth.RunAuthentication(kLoginSessionId, token)) {
		// Don't leak whether the token was malformed vs unknown vs
		// rejected by a custom callback — they all return 401 with
		// the same message.
		WriteJsonError(res, 401, "INVALID_TOKEN", "authentication failed");
		return;
	}

	auto principal_hex = flock_crypto::PrincipalIdHex(token);
	auto ttl_s = CookieTtlSec();
	auto cookie_value = auth.IssueCookie(principal_hex, ttl_s);

	// Cookie attributes per SPEC §7. HttpOnly: prevents JS read
	// (XSS exfiltration). SameSite=Strict: prevents cross-site
	// reuse. Path=/: cookie sent for every flock route. Secure:
	// only when the reverse proxy reports HTTPS upstream.
	std::ostringstream cookie;
	cookie << kCookieName << '=' << cookie_value
	       << "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" << ttl_s;
	if (RequestIsBehindHttps(req)) {
		cookie << "; Secure";
	}
	res.set_header("Set-Cookie", cookie.str());

	// Response body uses PrincipalAbbrev (first 8 hex chars) so we
	// don't echo the full principal_id back over the wire — same
	// rule as logging.
	auto expires_unix =
	    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
	                              std::chrono::system_clock::now().time_since_epoch())
	                              .count()) +
	    ttl_s;
	auto body = StringUtil::Format("{\"principal\":\"%s\",\"expires_at\":\"%s\"}",
	                               EscapeJsonString(flock_crypto::PrincipalAbbrev(principal_hex)),
	                               EscapeJsonString(IsoUtcFromUnix(expires_unix)));
	res.status = 200;
	res.set_content(body, "application/json");
}

void AuthHandlers::HandleLogout(const duckdb_httplib_openssl::Request &req,
                                duckdb_httplib_openssl::Response &res) {
	FlockHttpServer::ActiveRequestGuard guard(server);
	ApplyCorsHeaders(auth, req, res);

	// /auth/logout always returns 200 — never reveal whether the
	// caller had a valid cookie. The clear-cookie header is set
	// unconditionally so browsers drop whatever they had.
	std::ostringstream cookie;
	cookie << kCookieName
	       << "=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0";
	if (RequestIsBehindHttps(req)) {
		cookie << "; Secure";
	}
	res.set_header("Set-Cookie", cookie.str());

	// PR-4 logs-but-ignores ?destroy_sessions=true. Per SPEC §6 line
	// 668 this should destroy all DB sessions owned by the principal,
	// but SessionManager isn't principal-aware until PR-5 (when /sql
	// lands and the (principal, ui_conn) -> session_id map exists).
	// We accept the query param to avoid breaking forward-compat
	// clients; the actual destroy lands in PR-5.
	res.status = 200;
	res.set_content("{\"ok\":true}", "application/json");
}

void AuthHandlers::HandlePreflight(const duckdb_httplib_openssl::Request &req,
                                   duckdb_httplib_openssl::Response &res) {
	FlockHttpServer::ActiveRequestGuard guard(server);

	// Only emit CORS preflight headers when the request Origin is in
	// the allow-list. A bare 204 with no CORS headers tells the
	// browser the request is rejected (it'll surface as a CORS error
	// to the JS caller). This is the canonical way to "deny" a
	// preflight without revealing the policy.
	auto origin = req.get_header_value("Origin");
	if (!origin.empty()) {
		auto decision = auth.ResolveCorsOrigin(origin);
		if (decision.allowed) {
			res.set_header("Access-Control-Allow-Origin", decision.origin);
			res.set_header("Access-Control-Allow-Credentials", "true");
			res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
			res.set_header("Access-Control-Allow-Headers", kAllowedCorsHeaders);
			res.set_header("Access-Control-Max-Age", "600");
			res.set_header("Vary", "Origin");
		}
	}
	res.status = 204;
}

void AuthHandlers::Register(duckdb_httplib_openssl::Server &http) {
	auto *self = this;

	http.Post("/auth/login", [self](const duckdb_httplib_openssl::Request &req,
	                                 duckdb_httplib_openssl::Response &res) {
		self->HandleLogin(req, res);
	});

	http.Post("/auth/logout", [self](const duckdb_httplib_openssl::Request &req,
	                                  duckdb_httplib_openssl::Response &res) {
		self->HandleLogout(req, res);
	});

	http.Options("/auth/login", [self](const duckdb_httplib_openssl::Request &req,
	                                    duckdb_httplib_openssl::Response &res) {
		self->HandlePreflight(req, res);
	});

	http.Options("/auth/logout", [self](const duckdb_httplib_openssl::Request &req,
	                                     duckdb_httplib_openssl::Response &res) {
		self->HandlePreflight(req, res);
	});

	// /quack OPTIONS preflight — the actual POST handler lives in
	// QuackHandlers but the preflight policy is uniform, so we own
	// it here. SPEC §7 lists /quack as one of the routes that gets
	// CORS preflight when an allow-list is configured.
	http.Options("/quack", [self](const duckdb_httplib_openssl::Request &req,
	                               duckdb_httplib_openssl::Response &res) {
		self->HandlePreflight(req, res);
	});
}

} // namespace duckdb
