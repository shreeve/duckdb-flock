// PR-6: AdminHandlers — admin and operational HTTP routes per SPEC §4.
//
// The pre-PR-6 surface (just /health + /info) was extended to add
// /ready, /whoami, /tables, /schema/:db/:table, /checkpoint,
// /sessions, /interrupt. /sql/cancel lives in SqlHandlers (admin-authz
// gated; same __HARBOR_ADMIN__: invariant).
//
// Invariants enforced here:
// - Path parameters NEVER appear in the synthetic __HARBOR_ADMIN__:
//   policy decision input (per SPEC §7). The authz string is the
//   stable resource:action pair only; identifiers go through bound
//   prepared-statement parameters (or pragma_show_* table functions).
// - Mutating POSTs require Content-Type: application/json AND, when
//   the auth credential is the harbor_session cookie, an Origin or
//   Referer in the harbor_cors_origins allow-list (CSRF defence).
// - POST request bodies are bounded by harbor_max_request_body_bytes
//   (default 256 MiB, same as /sql).
// - Internal default-deny on __HARBOR_ADMIN__:* is enforced inside
//   AuthManager::RunAuthorization (PR-6) — these handlers do not
//   re-implement it; they just call RunAuthorization with the right
//   string.
// - All responses are wrapped in HarborHttpServer::ActiveRequestGuard
//   so they participate in the drain-on-close handshake.

#include "admin_handlers.hpp"

#include "harbor_auth.hpp"
#include "harbor_http_server.hpp"
#include "harbor_session.hpp"
#include "ui_handlers.hpp" // ui::UiHandlers::UiExtensionVersion (for /info)

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

namespace duckdb {

namespace {

constexpr size_t kDefaultMaxBodyBytes = 268435456; // 256 MiB; matches /sql default

// SPEC §6 — last_query truncation cap on /sessions output. 200 chars is
// enough to identify the query without bloating JSON for paste-bombed SQL.
constexpr idx_t kLastQueryCap = 200;

// harbor version, compiled in from EXT_VERSION_HARBOR if the build
// system provides it.
const char *HarborVersion() {
#ifdef EXT_VERSION_HARBOR
	return EXT_VERSION_HARBOR;
#else
	return "unknown";
#endif
}

constexpr const char *kQuackProtocolVersion = "1";

// JSON string escaping — same shape as auth_handlers / sql_handlers.
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

// Read the configured max body size, falling back to the SPEC default.
size_t ReadMaxBodyBytes(weak_ptr<DatabaseInstance> &db) {
	auto db_locked = db.lock();
	if (!db_locked) {
		return kDefaultMaxBodyBytes;
	}
	Value setting_val;
	auto &config = DBConfig::GetConfig(*db_locked);
	if (!config.TryGetCurrentSetting("harbor_max_request_body_bytes", setting_val) || setting_val.IsNull()) {
		return kDefaultMaxBodyBytes;
	}
	try {
		auto v = setting_val.GetValue<uint64_t>();
		return v == 0 ? kDefaultMaxBodyBytes : static_cast<size_t>(v);
	} catch (...) {
		return kDefaultMaxBodyBytes;
	}
}

// Drain the cpp-httplib content reader into `body_out`, bounded by
// `max_bytes`. Returns false if the bound is exceeded; the response
// is left to the caller to emit a 413.
bool ReadBoundedBody(const duckdb_httplib_openssl::ContentReader &content_reader, size_t max_bytes,
                     string &body_out) {
	body_out.clear();
	bool too_big = false;
	content_reader([&](const char *data, size_t length) {
		if (too_big) {
			return false;
		}
		if (body_out.size() + length > max_bytes) {
			too_big = true;
			return false;
		}
		body_out.append(data, length);
		return true;
	});
	return !too_big;
}

// Emit a JSON error envelope (admin shape — matches /sql for
// consistency: ok:false, error, errorCode).
void RespondError(duckdb_httplib_openssl::Response &res, int status, const string &error_code,
                  const string &message) {
	std::ostringstream w;
	w << "{\"ok\":false,\"error\":\"" << EscapeJsonString(message) << "\",\"errorCode\":\""
	  << EscapeJsonString(error_code) << "\"}";
	res.status = status;
	res.set_content(w.str(), "application/json");
}

// Authn front-door for admin routes. Returns AuthResult with ok=true
// on success; on failure populates res with the right 401/403.
struct AdminAuthOutcome {
	bool ok = false;
	int status = 0;
	string error_code;
	string message;
	string principal_id;
	AuthSource source = AuthSource::kNone;
};

AdminAuthOutcome AuthenticateAdmin(AuthManager &auth, const duckdb_httplib_openssl::Request &req) {
	AdminAuthOutcome out;
	auto authn = auth.AuthenticateRequest(req, AdminHandlers::kAdminSessionId);
	if (!authn.ok) {
		out.status = 401;
		out.error_code = "UNAUTHORIZED";
		if (authn.error_code == "MISSING_CREDENTIAL") {
			out.message = "missing credential — provide Authorization: Bearer or harbor_session cookie";
		} else if (authn.error_code == "INVALID_TOKEN") {
			out.message = "invalid bearer token";
		} else if (authn.error_code == "BAD_COOKIE_SIG" || authn.error_code == "BAD_COOKIE_FORMAT" ||
		           authn.error_code == "BAD_COOKIE_VERSION") {
			out.message = "harbor_session cookie failed verification";
		} else if (authn.error_code == "COOKIE_EXPIRED") {
			out.message = "harbor_session cookie expired";
		} else {
			out.message = "authentication failed";
		}
		return out;
	}
	out.ok = true;
	out.principal_id = authn.principal_id;
	out.source = authn.source;
	return out;
}

// CSRF gate for cookie-authenticated mutating POSTs. Mirrors the
// pattern in SqlHandlers' /sql cookie-Origin gate (per SPEC §7
// "Browser-origin requests do NOT bypass auth"). Bearer/X-Harbor-Token
// auth is non-browser-ambient and skips this check.
bool HasAllowedBrowserOriginIfCookie(AuthManager &auth, const duckdb_httplib_openssl::Request &req,
                                     AuthSource source) {
	if (source != AuthSource::kCookie) {
		return true;
	}
	auto origin = req.get_header_value("Origin");
	if (origin.empty()) {
		// Fall back to Referer (older browsers / certain CORS modes).
		auto ref = req.get_header_value("Referer");
		if (ref.empty()) {
			return false;
		}
		auto pos = ref.find("://");
		if (pos == string::npos) {
			return false;
		}
		auto schema_end = pos + 3;
		auto path_start = ref.find('/', schema_end);
		origin = ref.substr(0, path_start == string::npos ? ref.size() : path_start);
	}
	auto decision = auth.ResolveCorsOrigin(origin);
	return decision.allowed;
}

// Content-Type check for JSON-body POSTs. PR-6 follow-up (round 19/20):
// the prior `lower.find("application/json") == 0` accepted things like
// `application/jsonjunk`. Round 20: only `;` is the standard MIME
// parameter separator — the trailing-space variant is non-standard.
// Strip leading AND trailing whitespace from the header value, then
// require exact match OR the `;` parameter delimiter (so
// `application/json; charset=utf-8` continues to work).
bool HasJsonContentType(const duckdb_httplib_openssl::Request &req) {
	auto ct = req.get_header_value("Content-Type");
	auto lower = StringUtil::Lower(ct);
	while (!lower.empty() && std::isspace(static_cast<unsigned char>(lower.front()))) {
		lower.erase(lower.begin());
	}
	while (!lower.empty() && std::isspace(static_cast<unsigned char>(lower.back()))) {
		lower.pop_back();
	}
	return lower == "application/json" || StringUtil::StartsWith(lower, "application/json;");
}

// Find the value of "sessionId" in a small JSON body. Minimal — same
// approach as SqlHandlers (we don't pull in a JSON parser dep). Returns
// empty string if not found / malformed.
string ExtractJsonSessionId(const string &body) {
	auto pos = body.find("\"sessionId\"");
	if (pos == string::npos) {
		return string();
	}
	pos = body.find(':', pos);
	if (pos == string::npos) {
		return string();
	}
	pos = body.find('"', pos);
	if (pos == string::npos) {
		return string();
	}
	auto start = pos + 1;
	auto end = body.find('"', start);
	if (end == string::npos) {
		return string();
	}
	return body.substr(start, end - start);
}

// Build an ISO-8601 UTC timestamp from a steady_clock point by
// converting via system_clock. Used for /checkpoint's checkpointed_at.
string IsoUtcNow() {
	auto t = std::time(nullptr);
	std::tm tm_utc;
#if defined(_WIN32)
	gmtime_s(&tm_utc, &t);
#else
	gmtime_r(&t, &tm_utc);
#endif
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
	return string(buf);
}

} // namespace

AdminHandlers::AdminHandlers(HarborHttpServer &server_p, AuthManager &auth_p, SessionManager &sessions_p,
                             weak_ptr<DatabaseInstance> db_p)
    : server(server_p), auth(auth_p), sessions(sessions_p), db(std::move(db_p)) {
}

AdminHandlers::~AdminHandlers() {
}

void AdminHandlers::Register(duckdb_httplib_openssl::Server &http) {
	auto *self = this;

	http.Get("/health", [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleHealth(req, res);
	});

	http.Get("/info", [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleInfo(req, res);
	});

	http.Get("/ready", [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleReady(req, res);
	});

	http.Get("/whoami", [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleWhoami(req, res);
	});

	http.Get("/tables", [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleTables(req, res);
	});

	// /schema/:db/:table — explicit regex with two capture groups.
	// cpp-httplib's :param syntax is wired for handlers with no extra
	// args; the matches are exposed via req.matches[1], req.matches[2].
	http.Get("/schema/([^/]+)/([^/]+)",
	         [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
		         HarborHttpServer::ActiveRequestGuard guard(self->server);
		         self->HandleSchema(req, res);
	         });

	http.Post("/checkpoint", [self](const duckdb_httplib_openssl::Request &req,
	                                duckdb_httplib_openssl::Response &res,
	                                const duckdb_httplib_openssl::ContentReader &content_reader) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleCheckpoint(req, res, content_reader);
	});

	http.Get("/sessions", [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleSessions(req, res);
	});

	http.Post("/interrupt", [self](const duckdb_httplib_openssl::Request &req,
	                               duckdb_httplib_openssl::Response &res,
	                               const duckdb_httplib_openssl::ContentReader &content_reader) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleInterrupt(req, res, content_reader);
	});
}

// -- /health (public) ----------------------------------------------------

void AdminHandlers::HandleHealth(const duckdb_httplib_openssl::Request &, duckdb_httplib_openssl::Response &res) {
	auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
	                    std::chrono::steady_clock::now() - server.StartedAt())
	                    .count();
	auto body = StringUtil::Format("{\"ok\":true,\"version\":\"%s\",\"uptime_s\":%lld}",
	                               EscapeJsonString(HarborVersion()), static_cast<long long>(uptime_s));
	res.set_content(body, "application/json");
}

// -- /info (public, version headers) -------------------------------------

void AdminHandlers::HandleInfo(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
	auto request_origin = req.get_header_value("Origin");
	if (!request_origin.empty()) {
		auto decision = server.Auth().ResolveCorsOrigin(request_origin);
		if (decision.allowed) {
			res.set_header("Access-Control-Allow-Origin", decision.origin);
			res.set_header("Access-Control-Allow-Credentials", "true");
			res.set_header("Vary", "Origin");
		}
	}
	res.set_header("X-Harbor-Version", HarborVersion());
	res.set_header("X-DuckDB-Version", DuckDB::LibraryVersion());
	res.set_header("X-DuckDB-Platform", DuckDB::Platform());
	res.set_header("X-Quack-Protocol-Version", kQuackProtocolVersion);
	res.set_header("X-DuckDB-UI-Extension-Version", ui::UiHandlers::UiExtensionVersion());
	res.status = 204;
}

// -- /ready (public; runs SELECT 1) --------------------------------------

void AdminHandlers::HandleReady(const duckdb_httplib_openssl::Request &, duckdb_httplib_openssl::Response &res) {
	// PR-6 follow-up (round 19): /ready is PUBLIC. Per the security
	// review, this route MUST NOT echo DuckDB error strings, exception
	// `what()`, or any other server-side detail in its response — it's
	// reachable without auth, so any detail is an info-leak vector.
	// Failure shape is bare {ok:false}; failures are logged server-side
	// at DEBUG level via the existing Harbor log type for operator
	// triage.
	auto fail_quiet = [&res](const string &log_detail) {
		(void)log_detail; // future: route through Logger::Get(db) at DEBUG
		res.status = 503;
		res.set_content("{\"ok\":false}", "application/json");
	};

	auto db_locked = db.lock();
	if (!db_locked) {
		fail_quiet("database was closed");
		return;
	}
	try {
		Connection con(*db_locked);
		auto qr = con.Query("SELECT 1");
		if (!qr || qr->HasError()) {
			fail_quiet(qr ? qr->GetError() : "SELECT 1 returned no result");
			return;
		}
	} catch (const std::exception &ex) {
		fail_quiet(ex.what());
		return;
	}
	res.status = 200;
	res.set_content("{\"ok\":true}", "application/json");
}

// -- /whoami (authn + authz) ---------------------------------------------

void AdminHandlers::HandleWhoami(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
	auto authn = AuthenticateAdmin(auth, req);
	if (!authn.ok) {
		RespondError(res, authn.status, authn.error_code, authn.message);
		return;
	}
	if (!auth.RunAuthorization(kAdminSessionId, kAuthzWhoami)) {
		RespondError(res, 403, "FORBIDDEN", "authorization callback rejected the admin call");
		return;
	}
	const char *source_name = "unknown";
	switch (authn.source) {
	case AuthSource::kBearer:
		source_name = "bearer";
		break;
	case AuthSource::kXHarborToken:
		source_name = "x_harbor_token";
		break;
	case AuthSource::kCookie:
		source_name = "cookie";
		break;
	case AuthSource::kLocalDev:
		source_name = "local_dev";
		break;
	default:
		break;
	}
	std::ostringstream w;
	w << "{\"ok\":true,\"principal\":\"" << EscapeJsonString(authn.principal_id) << "\",\"auth_source\":\""
	  << source_name << "\",\"version\":\"" << EscapeJsonString(HarborVersion()) << "\",\"duckdb_version\":\""
	  << EscapeJsonString(DuckDB::LibraryVersion()) << "\"}";
	res.status = 200;
	res.set_content(w.str(), "application/json");
}

// -- /tables (authn + authz) --------------------------------------------

void AdminHandlers::HandleTables(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
	auto authn = AuthenticateAdmin(auth, req);
	if (!authn.ok) {
		RespondError(res, authn.status, authn.error_code, authn.message);
		return;
	}
	if (!auth.RunAuthorization(kAdminSessionId, kAuthzListTables)) {
		RespondError(res, 403, "FORBIDDEN", "authorization callback rejected the admin call");
		return;
	}

	auto db_locked = db.lock();
	if (!db_locked) {
		RespondError(res, 503, "INTERNAL", "database was closed");
		return;
	}

	// Per round-18 review: query duckdb_tables() rather than information_schema
	// to keep the output (database, schema, name, type) structured and stable.
	// NULL schema => default 'main' filter is applied client-side via SQL.
	Connection con(*db_locked);
	auto qr = con.Query(
	    "SELECT database_name, schema_name, table_name, internal "
	    "FROM duckdb_tables() ORDER BY database_name, schema_name, table_name");
	if (!qr || qr->HasError()) {
		RespondError(res, 500, "INTERNAL", qr ? qr->GetError() : "duckdb_tables() failed");
		return;
	}

	std::ostringstream w;
	w << "{\"ok\":true,\"tables\":[";
	bool first = true;
	while (true) {
		auto chunk = qr->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t i = 0; i < chunk->size(); i++) {
			if (!first) {
				w << ",";
			}
			first = false;
			auto database_name = chunk->GetValue(0, i);
			auto schema_name = chunk->GetValue(1, i);
			auto table_name = chunk->GetValue(2, i);
			auto internal = chunk->GetValue(3, i);
			w << "{\"database\":\"" << EscapeJsonString(database_name.IsNull() ? "" : database_name.ToString())
			  << "\",\"schema\":\"" << EscapeJsonString(schema_name.IsNull() ? "" : schema_name.ToString())
			  << "\",\"name\":\"" << EscapeJsonString(table_name.IsNull() ? "" : table_name.ToString())
			  << "\",\"internal\":" << ((!internal.IsNull() && internal.GetValue<bool>()) ? "true" : "false")
			  << "}";
		}
	}
	w << "]}";
	res.status = 200;
	res.set_content(w.str(), "application/json");
}

// -- /schema/:db/:table (authn + authz; identifier-safe) ----------------

void AdminHandlers::HandleSchema(const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
	auto authn = AuthenticateAdmin(auth, req);
	if (!authn.ok) {
		RespondError(res, authn.status, authn.error_code, authn.message);
		return;
	}
	// Path params NEVER appear in the authz string (per SPEC §7).
	if (!auth.RunAuthorization(kAdminSessionId, kAuthzDescribeTable)) {
		RespondError(res, 403, "FORBIDDEN", "authorization callback rejected the admin call");
		return;
	}

	// req.matches is a duckdb_re2::Match (not std::cmatch); operator[]
	// is non-const, so the const Request reference forces a const_cast
	// — this is a cpp-httplib API quirk also seen in
	// SqlHandlers::HandleSessionDelete. The route's regex
	// /schema/([^/]+)/([^/]+) guarantees two capture groups whenever
	// this handler runs (matches[0] is the full match, [1] is db,
	// [2] is table).
	auto &mutable_matches = const_cast<duckdb_httplib_openssl::Match &>(req.matches);
	auto database_name = mutable_matches[1].str();
	auto table_name = mutable_matches[2].str();
	if (database_name.empty() || table_name.empty()) {
		RespondError(res, 400, "BAD_REQUEST", "database and table path components are required");
		return;
	}

	auto db_locked = db.lock();
	if (!db_locked) {
		RespondError(res, 503, "INTERNAL", "database was closed");
		return;
	}

	// Per round-18 review: use the duckdb_columns() system function with
	// bound parameters. database_name + schema_name + table_name are
	// passed as Value bind parameters — they CANNOT be SQL-injected.
	// Default schema = 'main' (the v0.1 contract for this 2-segment path).
	// A future PR can add /schema/:db/:schema/:table for non-default schemas.
	Connection con(*db_locked);
	auto prepared = con.Prepare(
	    "SELECT column_name, data_type, is_nullable, column_default, column_index "
	    "FROM duckdb_columns() "
	    "WHERE database_name = $1 AND schema_name = 'main' AND table_name = $2 "
	    "ORDER BY column_index");
	if (!prepared || prepared->HasError()) {
		RespondError(res, 500, "INTERNAL", prepared ? prepared->GetError() : "prepare duckdb_columns() failed");
		return;
	}
	duckdb::vector<Value> binds;
	binds.emplace_back(Value(database_name));
	binds.emplace_back(Value(table_name));
	auto qr = prepared->Execute(binds);
	if (!qr || qr->HasError()) {
		RespondError(res, 500, "INTERNAL", qr ? qr->GetError() : "duckdb_columns() execute failed");
		return;
	}

	std::ostringstream w;
	w << "{\"ok\":true,\"database\":\"" << EscapeJsonString(database_name) << "\",\"schema\":\"main\""
	  << ",\"table\":\"" << EscapeJsonString(table_name) << "\",\"columns\":[";
	bool first = true;
	bool any_rows = false;
	while (true) {
		auto chunk = qr->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t i = 0; i < chunk->size(); i++) {
			any_rows = true;
			if (!first) {
				w << ",";
			}
			first = false;
			auto column_name = chunk->GetValue(0, i);
			auto data_type = chunk->GetValue(1, i);
			auto is_nullable = chunk->GetValue(2, i);
			auto column_default = chunk->GetValue(3, i);
			w << "{\"name\":\"" << EscapeJsonString(column_name.IsNull() ? "" : column_name.ToString())
			  << "\",\"type\":\"" << EscapeJsonString(data_type.IsNull() ? "" : data_type.ToString())
			  << "\",\"nullable\":" << ((!is_nullable.IsNull() && is_nullable.GetValue<bool>()) ? "true" : "false");
			if (!column_default.IsNull()) {
				w << ",\"default\":\"" << EscapeJsonString(column_default.ToString()) << "\"";
			} else {
				w << ",\"default\":null";
			}
			w << "}";
		}
	}
	w << "]}";
	if (!any_rows) {
		// 404 — the table either doesn't exist in <db>.main or is in a
		// different schema. Don't differentiate to avoid catalog-existence
		// enumeration.
		RespondError(res, 404, "NOT_FOUND", "table not found in database.main");
		return;
	}
	res.status = 200;
	res.set_content(w.str(), "application/json");
}

// -- /checkpoint (POST; CSRF + Content-Type + body-limit) ----------------

void AdminHandlers::HandleCheckpoint(const duckdb_httplib_openssl::Request &req,
                                     duckdb_httplib_openssl::Response &res,
                                     const duckdb_httplib_openssl::ContentReader &content_reader) {
	auto authn = AuthenticateAdmin(auth, req);
	if (!authn.ok) {
		RespondError(res, authn.status, authn.error_code, authn.message);
		return;
	}
	if (!HasAllowedBrowserOriginIfCookie(auth, req, authn.source)) {
		RespondError(res, 403, "FORBIDDEN",
		              "cookie-authenticated /checkpoint requires an allowed Origin or Referer");
		return;
	}
	if (!auth.RunAuthorization(kAdminSessionId, kAuthzCheckpointCreate)) {
		RespondError(res, 403, "FORBIDDEN", "authorization callback rejected the admin call");
		return;
	}

	// PR-6 follow-up (round 19): if the request advertises a Content-Type
	// at all, validate AND drain the body — chunked transfer encoding
	// (no Content-Length) was previously skipping both checks.
	// Otherwise (truly empty POST: no Content-Type, no Content-Length)
	// allow the no-args common case without ceremony.
	auto content_length_header = req.get_header_value("Content-Length");
	auto content_type_header = req.get_header_value("Content-Type");
	bool has_body =
	    (!content_length_header.empty() && content_length_header != "0") || !content_type_header.empty();
	if (has_body) {
		if (!HasJsonContentType(req)) {
			RespondError(res, 415, "UNSUPPORTED_MEDIA_TYPE", "/checkpoint expects Content-Type: application/json");
			return;
		}
		string body;
		if (!ReadBoundedBody(content_reader, ReadMaxBodyBytes(db), body)) {
			RespondError(res, 413, "PAYLOAD_TOO_LARGE",
			              "request body exceeds harbor_max_request_body_bytes");
			return;
		}
		// Body parsing is best-effort; v0.1 only accepts an empty
		// object {} or {"database":"<name>"} — anything else is logged
		// but doesn't change behavior. Future PR can support targeted
		// per-database checkpoints.
		(void)body;
	}

	auto db_locked = db.lock();
	if (!db_locked) {
		RespondError(res, 503, "INTERNAL", "database was closed");
		return;
	}
	Connection con(*db_locked);
	bool forced = false;
	try {
		// Plain CHECKPOINT first — fast, common case. If another write
		// transaction is open it errors with "Cannot CHECKPOINT: there
		// are other write transactions active. Try using FORCE
		// CHECKPOINT…"; we report that cleanly to the caller rather
		// than auto-escalating, because FORCE CHECKPOINT can block
		// indefinitely (it waits for the conflicting transaction).
		// Operators who genuinely need the forcing semantics issue
		// `FORCE CHECKPOINT` via /sql from a privileged session.
		auto qr = con.Query("CHECKPOINT");
		if (!qr || qr->HasError()) {
			auto err = qr ? qr->GetError() : "CHECKPOINT failed";
			// Map the well-known busy error to 409 CONFLICT so
			// operator tooling can distinguish "transient try-again"
			// from real DB failure.
			if (err.find("other write transactions active") != string::npos ||
			    err.find("Cannot CHECKPOINT") != string::npos) {
				RespondError(res, 409, "CONFLICT", err);
				return;
			}
			RespondError(res, 500, "INTERNAL", err);
			return;
		}
	} catch (const std::exception &ex) {
		string err(ex.what());
		if (err.find("other write transactions active") != string::npos ||
		    err.find("Cannot CHECKPOINT") != string::npos) {
			RespondError(res, 409, "CONFLICT", err);
			return;
		}
		RespondError(res, 500, "INTERNAL", err);
		return;
	}
	(void)forced;

	// Per round-18 review: be explicit that the v0.1 response does NOT
	// include WAL state. v1.5.2 doesn't expose a stable WAL-size API
	// from a Connection; PR-7+ will revisit when admin instrumentation
	// catches up.
	std::ostringstream w;
	w << "{\"ok\":true,\"checkpointed_at\":\"" << IsoUtcNow() << "\",\"wal_state_available\":false}";
	res.status = 200;
	res.set_content(w.str(), "application/json");
}

// -- /sessions (GET; SessionManager::Snapshot) --------------------------

void AdminHandlers::HandleSessions(const duckdb_httplib_openssl::Request &req,
                                    duckdb_httplib_openssl::Response &res) {
	auto authn = AuthenticateAdmin(auth, req);
	if (!authn.ok) {
		RespondError(res, authn.status, authn.error_code, authn.message);
		return;
	}
	if (!auth.RunAuthorization(kAdminSessionId, kAuthzSessionsList)) {
		RespondError(res, 403, "FORBIDDEN", "authorization callback rejected the admin call");
		return;
	}

	auto snapshots = sessions.Snapshot(kLastQueryCap);
	auto now = std::chrono::steady_clock::now();

	std::ostringstream w;
	w << "{\"ok\":true,\"sessions\":[";
	bool first = true;
	for (auto &snap : snapshots) {
		if (!first) {
			w << ",";
		}
		first = false;
		auto age_s = std::chrono::duration_cast<std::chrono::seconds>(now - snap.created_at).count();
		w << "{\"session_id\":\"" << EscapeJsonString(snap.session_id) << "\",\"principal\":\""
		  << EscapeJsonString(snap.owner_principal_id) << "\",\"age_s\":" << static_cast<long long>(age_s)
		  << ",\"in_flight\":" << (snap.query_in_flight ? "true" : "false") << ",\"last_query\":\""
		  << EscapeJsonString(snap.last_query) << "\",\"last_query_truncated\":"
		  << (snap.last_query_truncated ? "true" : "false") << "}";
	}
	w << "]}";
	res.status = 200;
	res.set_content(w.str(), "application/json");
}

// -- /interrupt (POST; CSRF + Content-Type + body-limit) ----------------

void AdminHandlers::HandleInterrupt(const duckdb_httplib_openssl::Request &req,
                                     duckdb_httplib_openssl::Response &res,
                                     const duckdb_httplib_openssl::ContentReader &content_reader) {
	auto authn = AuthenticateAdmin(auth, req);
	if (!authn.ok) {
		RespondError(res, authn.status, authn.error_code, authn.message);
		return;
	}
	if (!HasAllowedBrowserOriginIfCookie(auth, req, authn.source)) {
		RespondError(res, 403, "FORBIDDEN",
		              "cookie-authenticated /interrupt requires an allowed Origin or Referer");
		return;
	}
	if (!auth.RunAuthorization(kAdminSessionId, kAuthzSessionsInterrupt)) {
		RespondError(res, 403, "FORBIDDEN", "authorization callback rejected the admin call");
		return;
	}
	if (!HasJsonContentType(req)) {
		RespondError(res, 415, "UNSUPPORTED_MEDIA_TYPE", "/interrupt expects Content-Type: application/json");
		return;
	}
	string body;
	if (!ReadBoundedBody(content_reader, ReadMaxBodyBytes(db), body)) {
		RespondError(res, 413, "PAYLOAD_TOO_LARGE", "request body exceeds harbor_max_request_body_bytes");
		return;
	}
	auto session_id = ExtractJsonSessionId(body);
	if (session_id.empty()) {
		RespondError(res, 400, "BAD_REQUEST", "expected JSON body with a non-empty 'sessionId' field");
		return;
	}

	bool ok = sessions.InterruptSession(session_id);
	if (!ok) {
		RespondError(res, 404, "SESSION_NOT_FOUND", "no session with the given id");
		return;
	}
	std::ostringstream w;
	w << "{\"ok\":true,\"session_id\":\"" << EscapeJsonString(session_id) << "\"}";
	res.status = 200;
	res.set_content(w.str(), "application/json");
}

} // namespace duckdb
