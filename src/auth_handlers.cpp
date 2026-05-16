#include "auth_handlers.hpp"

#include "harbor_auth.hpp"
#include "harbor_crypto.hpp"
#include "harbor_http_server.hpp"
#include "harbor_session.hpp"

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
    "Authorization, Content-Type, X-Harbor-Token, X-Harbor-Session-Id, Accept";

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

AuthHandlers::AuthHandlers(HarborHttpServer &server_p, AuthManager &auth_p, SessionManager &sessions_p)
    : server(server_p), auth(auth_p), sessions(sessions_p) {
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
	if (!config.TryGetCurrentSetting("harbor_auth_cookie_ttl_s", setting_val) || setting_val.IsNull()) {
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
	// Two signals (per round-12 review) so an operator misconfigured
	// reverse proxy that strips X-Forwarded-Proto still gets Secure
	// cookies if the browser believes the page is HTTPS:
	//
	//   1. X-Forwarded-Proto: https (preferred — explicit proxy contract)
	//   2. Origin: https://...      (fallback — the browser-asserted scheme)
	//
	// Either signal flips Secure on. Belt-and-suspenders: it's safer
	// to over-set Secure than to omit it. (A Secure cookie sent to an
	// HTTP-only origin is simply not sent back by the browser; no
	// breakage, the user just re-logs-in.)
	//
	// We deliberately do NOT use either signal for AUTHORIZATION
	// decisions — that path stays gated on the actual auth credential.
	// X-Forwarded-Proto is forgeable; Origin is too in non-browser
	// contexts. Secure is a UI-side hint, not an authz bit.
	auto xfp = req.get_header_value("X-Forwarded-Proto");
	if (!xfp.empty() && StringUtil::Lower(xfp) == "https") {
		return true;
	}
	auto origin = req.get_header_value("Origin");
	if (!origin.empty() && origin.compare(0, 8, "https://") == 0) {
		return true;
	}
	return false;
}

void AuthHandlers::HandleLogin(const duckdb_httplib_openssl::Request &req,
                               duckdb_httplib_openssl::Response &res) {
	HarborHttpServer::ActiveRequestGuard guard(server);
	ApplyCorsHeaders(auth, req, res);

	// Token resolution precedence on /auth/login (different from the
	// standard request-auth precedence — for /auth/login itself, the
	// JSON body is the canonical form because the login page POSTs
	// the user-pasted token there):
	//
	//   1. Authorization: Bearer <token>
	//   2. X-Harbor-Token: <token>
	//   3. JSON body {"token":"<token>"}
	//
	// We accept all three so curl users, browser users, and
	// programmatic clients can all reach this endpoint naturally.
	string token;
	auto auth_header = req.get_header_value("Authorization");
	auto x_harbor = req.get_header_value("X-Harbor-Token");
	if (!auth_header.empty() && auth_header.size() > 7 &&
	    auth_header.compare(0, 7, "Bearer ") == 0) {
		token = TrimCopy(auth_header.substr(7));
	} else if (!x_harbor.empty()) {
		token = TrimCopy(x_harbor);
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
		               "/auth/login requires Authorization, X-Harbor-Token, or {\"token\":\"...\"} body");
		return;
	}

	if (!auth.RunAuthentication(kLoginSessionId, token)) {
		// Don't leak whether the token was malformed vs unknown vs
		// rejected by a custom callback — they all return 401 with
		// the same message.
		WriteJsonError(res, 401, "INVALID_TOKEN", "authentication failed");
		return;
	}

	auto principal_hex = harbor_crypto::PrincipalIdHex(token);
	auto ttl_s = CookieTtlSec();
	auto cookie_value = auth.IssueCookie(principal_hex, ttl_s);

	// Cookie attributes per SPEC §7. HttpOnly: prevents JS read
	// (XSS exfiltration). SameSite=Strict: prevents cross-site
	// reuse. Path=/: cookie sent for every harbor route. Secure:
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
	                               EscapeJsonString(harbor_crypto::PrincipalAbbrev(principal_hex)),
	                               EscapeJsonString(IsoUtcFromUnix(expires_unix)));
	res.status = 200;
	res.set_content(body, "application/json");
}

void AuthHandlers::HandleLogout(const duckdb_httplib_openssl::Request &req,
                                duckdb_httplib_openssl::Response &res) {
	HarborHttpServer::ActiveRequestGuard guard(server);
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

	// PR-5: ?destroy_sessions=true destroys all DB sessions owned by
	// the authenticated principal (per SPEC §6 logout table). We
	// authenticate first to learn the principal_id; if the caller is
	// unauthenticated (no valid cookie/bearer) we silently skip the
	// destroy (and still return 200 to avoid leaking auth state).
	auto destroy_param = req.get_param_value("destroy_sessions");
	idx_t destroyed = 0;
	if (StringUtil::Lower(destroy_param) == "true" || destroy_param == "1") {
		auto authn = auth.AuthenticateRequest(req, AuthHandlers::kLoginSessionId);
		if (authn.ok && !authn.principal_id.empty()) {
			destroyed = sessions.DestroyAllOwnedBy(authn.principal_id);
		}
	}

	res.status = 200;
	if (destroyed > 0) {
		auto body = StringUtil::Format("{\"ok\":true,\"destroyedSessions\":%llu}",
		                               static_cast<unsigned long long>(destroyed));
		res.set_content(body, "application/json");
	} else {
		res.set_content("{\"ok\":true}", "application/json");
	}
}

void AuthHandlers::HandlePreflight(const duckdb_httplib_openssl::Request &req,
                                   duckdb_httplib_openssl::Response &res) {
	HarborHttpServer::ActiveRequestGuard guard(server);

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
			res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
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

	// Defensive 405 on GET /auth/{login,logout} — without these, a
	// browser GET would fall through to the UI catch-all, which would
	// 401 (path != "/"). 405 is the more honest answer ("method not
	// allowed on a route that exists") and matches what curl tooling
	// expects. Per round-12 review: don't let route-order semantics
	// be the source of correctness.
	auto method_not_allowed = [self](const duckdb_httplib_openssl::Request &req,
	                                  duckdb_httplib_openssl::Response &res) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		res.set_header("Allow", "POST, OPTIONS");
		WriteJsonError(res, 405, "METHOD_NOT_ALLOWED",
		               StringUtil::Format("only POST is allowed on %s", req.path));
	};
	http.Get("/auth/login", method_not_allowed);
	http.Get("/auth/logout", method_not_allowed);

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

	// /sql OPTIONS preflight — same pattern. The actual POST handler
	// lives in SqlHandlers (PR-5). SPEC §7 lists /sql in the preflight
	// route table.
	http.Options("/sql", [self](const duckdb_httplib_openssl::Request &req,
	                             duckdb_httplib_openssl::Response &res) {
		self->HandlePreflight(req, res);
	});

	// Explicit SQL-session routes need their own browser preflight
	// coverage; CORS preflight is per-path, so OPTIONS /sql does not
	// cover /sql/sessions/new or /sql/sessions/<id>.
	http.Options("/sql/sessions/new", [self](const duckdb_httplib_openssl::Request &req,
	                                          duckdb_httplib_openssl::Response &res) {
		self->HandlePreflight(req, res);
	});
	http.Options(R"(^/sql/sessions/([^/]+)$)", [self](const duckdb_httplib_openssl::Request &req,
	                                                   duckdb_httplib_openssl::Response &res) {
		self->HandlePreflight(req, res);
	});
}

} // namespace duckdb
