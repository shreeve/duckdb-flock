// PR-5: SqlHandlers — POST /sql, /sql/sessions/new, /sql/sessions/:id.
//
// Per SPEC §5.2-5.4. Streaming NDJSON default; one-shot JSON for tiny
// queries; session-bound or ephemeral; per-statement authz; mid-stream
// error encoding via the chunk encoder; buffer-before-write discipline
// (every chunk is built into a JsonWriter and written to the network
// in one sink.write call).

#include "sql_handlers.hpp"

#include "harbor_auth.hpp"
#include "harbor_crypto.hpp"
#include "harbor_http_server.hpp"
#include "harbor_query_timeout.hpp"
#include "harbor_session.hpp"
#include "sql_chunk_encoder.hpp"
#include "sql_json_writer.hpp"
#include "sql_param_decoder.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"

#include <chrono>
#include <memory>
#include <sstream>

namespace duckdb {

namespace {

namespace fjs = duckdb::harbor_sql;

constexpr size_t kDefaultMaxBodyBytes = 268435456; // 256 MiB; matches SPEC §6 default

// Read a UBIGINT setting with a fallback default.
uint64_t ReadUbigintSetting(weak_ptr<DatabaseInstance> &db, const string &name, uint64_t fallback) {
	auto db_locked = db.lock();
	if (!db_locked) {
		return fallback;
	}
	Value setting_val;
	auto &config = DBConfig::GetConfig(*db_locked);
	if (!config.TryGetCurrentSetting(name, setting_val) || setting_val.IsNull()) {
		return fallback;
	}
	try {
		return setting_val.GetValue<uint64_t>();
	} catch (...) {
		return fallback;
	}
}

// Choose response shape from Accept header.
enum class ResponseShape : uint8_t { kNdjsonRowMode, kNdjsonChunkMode, kOneShotJson };

ResponseShape ChooseShape(const string &accept_header) {
	auto lower = StringUtil::Lower(accept_header);
	if (lower.find("application/json") != string::npos &&
	    lower.find("application/x-ndjson") == string::npos) {
		return ResponseShape::kOneShotJson;
	}
	if (lower.find("shape=chunk") != string::npos) {
		return ResponseShape::kNdjsonChunkMode;
	}
	return ResponseShape::kNdjsonRowMode;
}

// Map a DuckDB error message / type to a stable SPEC §5.2 errorCode.
string ClassifyError(const string &message) {
	auto lower = StringUtil::Lower(message);
	if (lower.find("syntax error") != string::npos || lower.find("parser error") != string::npos) {
		return "SQL_SYNTAX";
	}
	if (lower.find("table") != string::npos && lower.find("does not exist") != string::npos) {
		return "TABLE_NOT_FOUND";
	}
	if (lower.find("column") != string::npos && lower.find("not found") != string::npos) {
		return "TABLE_NOT_FOUND"; // close enough for our error matrix
	}
	if (lower.find("type") != string::npos &&
	    (lower.find("mismatch") != string::npos || lower.find("conversion") != string::npos)) {
		return "TYPE_ERROR";
	}
	if (lower.find("io error") != string::npos || lower.find("ioexception") != string::npos) {
		return "IO_ERROR";
	}
	if (lower.find("session limit") != string::npos) {
		return "SESSION_LIMIT";
	}
	return "SQL_ERROR";
}

// Emit a JSON error envelope (one-shot path, NOT mid-stream).
void RespondError(duckdb_httplib_openssl::Response &res, int status, const string &error_code,
                   const string &message, const string &session_id = string()) {
	fjs::JsonWriter w;
	w.BeginObject();
	w.KeyBool("ok", false);
	w.KeyString("error", message);
	w.KeyString("errorCode", error_code);
	if (!session_id.empty()) {
		w.KeyString("sessionId", session_id);
	}
	w.EndObject();
	res.status = status;
	res.set_content(w.Take(), "application/json");
}

// Authenticate per SPEC §7 AND check that the SQL doesn't pretend to be
// admin-synthetic.
struct AuthOutcome {
	bool ok = false;
	string principal_id;
	AuthSource source = AuthSource::kNone;
	string error_code;
	string message;
	int status = 401;
};

AuthOutcome AuthenticateForSql(AuthManager &auth, const duckdb_httplib_openssl::Request &req,
                                const string &synthetic_sid) {
	AuthOutcome out;
	auto result = auth.AuthenticateRequest(req, synthetic_sid);
	if (!result.ok) {
		out.ok = false;
		out.status = 401;
		out.error_code = result.error_code.empty() ? "UNAUTHORIZED" : result.error_code;
		out.message = "authentication failed";
		return out;
	}
	out.ok = true;
	out.principal_id = result.principal_id;
	out.source = result.source;
	return out;
}

bool IsJsonContentType(const string &content_type) {
	if (content_type.empty()) {
		return false;
	}
	auto lower = StringUtil::Lower(content_type);
	// Allow parameters (`application/json; charset=utf-8`).
	return lower == "application/json" || StringUtil::StartsWith(lower, "application/json;");
}

bool IsSameOriginAllowed(const HarborHttpServer &server, const string &origin) {
	if (origin.empty()) {
		return false;
	}
	auto port = server.ListenUri().Port();
	if (origin == StringUtil::Format("http://localhost:%d", port) ||
	    origin == StringUtil::Format("http://127.0.0.1:%d", port) ||
	    origin == StringUtil::Format("http://[::1]:%d", port)) {
		return true;
	}
	auto host = server.ListenUri().Host();
	if (!host.empty() && host != "0.0.0.0") {
		auto host_form = server.ListenUri().IPv6() ? StringUtil::Format("[%s]", host) : host;
		return origin == StringUtil::Format("http://%s:%d", host_form, port);
	}
	return false;
}

bool HasAllowedBrowserOrigin(const HarborHttpServer &server, AuthManager &auth,
                             const duckdb_httplib_openssl::Request &req) {
	auto origin = req.get_header_value("Origin");
	if (!origin.empty()) {
		if (IsSameOriginAllowed(server, origin)) {
			return true;
		}
		auto decision = auth.ResolveCorsOrigin(origin);
		return decision.allowed;
	}
	// Fallback for older forms / non-fetch browser submissions.
	auto referer = req.get_header_value("Referer");
	if (!referer.empty()) {
		auto slash = referer.find('/', referer.find("://") == string::npos ? 0 : referer.find("://") + 3);
		auto referer_origin = slash == string::npos ? referer : referer.substr(0, slash);
		if (IsSameOriginAllowed(server, referer_origin)) {
			return true;
		}
		auto decision = auth.ResolveCorsOrigin(referer_origin);
		return decision.allowed;
	}
	return false;
}

bool ReadRequestBodyWithLimit(const duckdb_httplib_openssl::ContentReader &content_reader, uint64_t max_body,
                              string &out, string &error) {
	out.clear();
	bool too_large = false;
	content_reader([&](const char *data, size_t data_length) {
		if (too_large) {
			return false;
		}
		if (out.size() + data_length > max_body) {
			too_large = true;
			error = StringUtil::Format("request body exceeds harbor_max_request_body_bytes=%llu",
			                           (unsigned long long)max_body);
			return false;
		}
		out.append(data, data_length);
		return true;
	});
	return !too_large;
}

// Apply CORS allow-list to the response if the request Origin matches.
void ApplyCors(AuthManager &auth, const duckdb_httplib_openssl::Request &req,
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

// Resolve a session id for a /sql request: returns either an OWNED
// long-lived session (from SessionManager.LookupOwnedSession) or
// nullptr if the request specified no sessionId. NEVER returns
// "session not found" — caller checks for nullptr-with-no-id-given
// vs nullptr-with-id-given.
struct SessionResolution {
	bool ok = false;
	bool ephemeral = false;             // true iff no sessionId provided; caller creates a fresh local Connection
	shared_ptr<HarborSession> session;   // populated iff !ephemeral && ok
	string error_code;
	string message;
	int status = 0;
};

SessionResolution ResolveSession(SessionManager &sessions, const string &session_id, const string &principal_id) {
	SessionResolution r;
	if (session_id.empty()) {
		r.ok = true;
		r.ephemeral = true;
		return r;
	}
	auto sess = sessions.LookupOwnedSession(session_id, principal_id);
	if (!sess) {
		r.ok = false;
		r.status = 404;
		r.error_code = "SESSION_NOT_FOUND";
		r.message = "no such session for this principal";
		return r;
	}
	r.ok = true;
	r.ephemeral = false;
	r.session = std::move(sess);
	return r;
}

// Streaming context that must outlive the route lambda. Owned by a
// shared_ptr captured by the chunked-content provider closure.
//
// IMPORTANT: this struct holds the session lock guard for the entire
// streaming lifetime (per SPEC §6 "the session mutex is held for the
// full duration of any request that touches it, including the
// streaming response body"). The lock_guard is moved into the struct
// at request-handler entry; released only when the struct is destroyed.
struct StreamingCtx {
	std::shared_ptr<HarborHttpServer::ActiveRequestGuard> guard; // request-counted for drain
	shared_ptr<HarborSession> session;                            // null in ephemeral mode
	unique_ptr<Connection> ephemeral_connection;                 // populated in ephemeral mode
	std::unique_lock<mutex> session_lock;                        // holds the session mutex (non-ephemeral)
	unique_ptr<QueryResult> result;
	unique_ptr<fjs::SqlChunkEncoder> encoder;
	string session_id_for_response;
	idx_t row_count = 0;
	idx_t max_rows = 0;       // 0 = unlimited
	bool truncated = false;
	bool schema_emitted = false;
	bool end_emitted = false;
	bool error_emitted = false;
	std::chrono::steady_clock::time_point started_at;
	bool chunk_mode = false;
	// PR-7b — query-timeout enforcement for the streaming response.
	// Exactly one of these is populated based on whether the request
	// has a SessionManager-tracked session (sessionful → guard) or an
	// ephemeral Connection (ephemeral → watchdog). Their destructors
	// run before any other StreamingCtx member, doing the right
	// cleanup (guard clears query_in_flight + deadline; watchdog
	// signals done + joins the timer thread). Replaces the PR-6
	// manual ~StreamingCtx body that only flipped query_in_flight.
	unique_ptr<QueryExecutionGuard> session_guard;
	unique_ptr<QueryTimeoutWatchdog> ephemeral_watchdog;
	// PR-7b — return the actual interrupt cause that fired (if any),
	// so the streaming provider's catch + empty-chunk paths can
	// classify QUERY_TIMEOUT vs QUERY_CANCELLED vs natural end.
	// Round 22 (GPT-5.5): the prior bool TimedOut() was insufficient
	// — DuckDB's StreamingQueryResult returns empty (not an exception)
	// on Interrupt regardless of WHO fired it. With only TimedOut()
	// the empty-chunk branch would emit `{"type":"end"}` for a
	// USER_CANCEL'd stream, falsely indicating success.
	//
	// Sessionful path: session_guard knows TIMEOUT (from
	// timed_out_generation) and USER_CANCEL/DISCONNECT (from
	// interrupt_cause atomic).
	// Ephemeral path: only TIMEOUT applies (USER_CANCEL targets a
	// sessionId via SessionManager which ephemerals are not in;
	// DISCONNECT path doesn't call Interrupt() in v0.1).
	InterruptCause Cause() const {
		if (session_guard) {
			return session_guard->Cause();
		}
		if (ephemeral_watchdog && ephemeral_watchdog->TimedOut()) {
			return InterruptCause::TIMEOUT;
		}
		return InterruptCause::NONE;
	}
	bool TimedOut() const {
		return Cause() == InterruptCause::TIMEOUT;
	}
};

// Build the row/end/error chunk, push it to the sink in one go.
bool WriteOneLine(duckdb_httplib_openssl::DataSink &sink, fjs::JsonWriter &line) {
	auto str = line.Take();
	str.push_back('\n');
	return sink.write(str.data(), str.size());
}

void EmitStreamingErrorSafe(StreamingCtx &ctx, duckdb_httplib_openssl::DataSink &sink,
                            const string &error_code, const string &message) {
	try {
		fjs::JsonWriter w;
		ctx.encoder->EmitError(w, error_code, message);
		WriteOneLine(sink, w);
	} catch (...) {
		// Nothing useful left to do: even the error encoder failed.
		// Close the stream cleanly so httplib doesn't keep calling us.
	}
	sink.done();
	ctx.error_emitted = true;
}

} // namespace

// ---------- ctor / dtor ----------

SqlHandlers::SqlHandlers(HarborHttpServer &server_p, AuthManager &auth_p, SessionManager &sessions_p,
                         weak_ptr<DatabaseInstance> db_p)
    : server(server_p), auth(auth_p), sessions(sessions_p), db(std::move(db_p)) {
}

SqlHandlers::~SqlHandlers() = default;

// ---------- POST /sql ----------

void SqlHandlers::HandleSql(const duckdb_httplib_openssl::Request &req,
                              duckdb_httplib_openssl::Response &res,
                              const duckdb_httplib_openssl::ContentReader &content_reader) {
	HarborHttpServer::ActiveRequestGuard sync_guard(server);
	ApplyCors(auth, req, res);

	if (!IsJsonContentType(req.get_header_value("Content-Type"))) {
		RespondError(res, 415, "BAD_REQUEST", "/sql requires Content-Type: application/json");
		return;
	}

	// Body size guard while streaming request bytes from httplib. Do
	// NOT rely on req.body.size(): by then httplib has already buffered
	// the full body, which defeats harbor_max_request_body_bytes as a
	// memory DoS guard (round-16 GPT-5.5 catch).
	auto max_body = ReadUbigintSetting(db, "harbor_max_request_body_bytes", kDefaultMaxBodyBytes);
	string body;
	string body_error;
	if (!ReadRequestBodyWithLimit(content_reader, max_body, body, body_error)) {
		RespondError(res, 413, "PAYLOAD_TOO_LARGE", body_error);
		return;
	}

	// Parse request body.
	auto parsed = fjs::SqlParamDecoder::ParseRequest(body);
	if (!parsed.ok) {
		RespondError(res, 400, "BAD_REQUEST", parsed.error);
		return;
	}

	// Allow X-Harbor-Session-Id header as alternative to body's sessionId.
	if (parsed.session_id.empty()) {
		auto h = req.get_header_value("X-Harbor-Session-Id");
		if (!h.empty()) {
			parsed.session_id = h;
		}
	}

	// SPEC §7: reject __HARBOR_ADMIN__:* synthetic admin SQL from clients.
	string trimmed_sql = parsed.sql;
	StringUtil::Trim(trimmed_sql);
	if (StringUtil::StartsWith(trimmed_sql, "__HARBOR_ADMIN__:")) {
		RespondError(res, 400, "BAD_REQUEST", "SQL must not begin with __HARBOR_ADMIN__: (reserved prefix)");
		return;
	}

	// Authenticate.
	auto authn = AuthenticateForSql(auth, req, kAdminSessionId);
	if (!authn.ok) {
		RespondError(res, authn.status, authn.error_code, authn.message);
		return;
	}
	// Cookie-authenticated SQL execution is browser-origin sensitive:
	// SameSite=Strict is helpful but not our only CSRF control. Require
	// same-origin or an entry in harbor_cors_origins whenever the
	// credential source is the ambient harbor_session cookie. Explicit
	// Bearer/X-Harbor-Token clients are not browser-ambient and don't
	// need this Origin gate.
	if (authn.source == AuthSource::kCookie && !HasAllowedBrowserOrigin(server, auth, req)) {
		RespondError(res, 403, "FORBIDDEN", "cookie-authenticated /sql requests require an allowed Origin or Referer");
		return;
	}

	// Resolve session: either an existing owned session or fresh ephemeral.
	auto resolved = ResolveSession(sessions, parsed.session_id, authn.principal_id);
	if (!resolved.ok) {
		RespondError(res, resolved.status, resolved.error_code, resolved.message, parsed.session_id);
		return;
	}

	// Per-session mutex: try-lock for non-ephemeral sessions. If locked,
	// 409 SESSION_BUSY immediately (don't wait, per SPEC §6). We use
	// std::unique_lock so we can move it into the streaming context
	// later without releasing.
	std::unique_lock<mutex> session_lock;
	if (!resolved.ephemeral) {
		session_lock = std::unique_lock<mutex>(resolved.session->lock, std::try_to_lock);
		if (!session_lock.owns_lock()) {
			RespondError(res, 409, "SESSION_BUSY", "another request is using this session", parsed.session_id);
			return;
		}
	}

	// Resolve the Connection to use. For ephemeral sessions, build a
	// fresh local Connection; for owned sessions, use the bound one.
	auto db_locked = db.lock();
	if (!db_locked) {
		RespondError(res, 503, "INTERNAL", "database was closed");
		return;
	}
	unique_ptr<Connection> ephemeral_conn;
	Connection *conn = nullptr;
	if (resolved.ephemeral) {
		ephemeral_conn = make_uniq<Connection>(*db_locked);
		ephemeral_conn->context->config.enable_progress_bar = false;
		conn = ephemeral_conn.get();
	} else {
		conn = resolved.session->duckdb_connection.get();
	}

	// Per-statement authorization.
	if (!auth.RunAuthorization(parsed.session_id.empty() ? kAdminSessionId : parsed.session_id, parsed.sql)) {
		RespondError(res, 403, "FORBIDDEN", "authorization callback rejected the query", parsed.session_id);
		return;
	}

	// Multi-statement guard. ExtractStatements parses; catch and
	// classify exceptions as SQL_SYNTAX.
	vector<unique_ptr<SQLStatement>> statements;
	try {
		statements = conn->ExtractStatements(parsed.sql);
	} catch (const std::exception &ex) {
		RespondError(res, 400, "SQL_SYNTAX", ex.what(), parsed.session_id);
		return;
	}
	if (statements.size() == 0) {
		RespondError(res, 400, "BAD_REQUEST", "SQL contains no statements", parsed.session_id);
		return;
	}
	if (statements.size() > 1) {
		RespondError(res, 400, "BAD_REQUEST",
		              "multi-statement requests not supported on /sql; use /sql/sessions/new for transactions",
		              parsed.session_id);
		return;
	}

	// Reject ephemeral transactions early (SPEC §5.2).
	if (resolved.ephemeral) {
		string upper_sql = parsed.sql;
		StringUtil::Trim(upper_sql);
		upper_sql = StringUtil::Upper(upper_sql);
		if (StringUtil::StartsWith(upper_sql, "BEGIN") ||
		    StringUtil::StartsWith(upper_sql, "START TRANSACTION")) {
			RespondError(res, 400, "BAD_REQUEST",
			              "transactions require an explicit sessionId; create one with POST /sql/sessions/new");
			return;
		}
	}

	// Prepare + decode params + execute. Capture the statement type and
	// RETURNING status BEFORE the unique_ptr is moved into Prepare, so
	// we can choose "select" vs "write" response shape after execute.
	auto stmt_type = statements[0]->type;
	bool has_returning = false;
	switch (stmt_type) {
	case StatementType::INSERT_STATEMENT:
		has_returning = !statements[0]->Cast<InsertStatement>().returning_list.empty();
		break;
	case StatementType::UPDATE_STATEMENT:
		has_returning = !statements[0]->Cast<UpdateStatement>().returning_list.empty();
		break;
	case StatementType::DELETE_STATEMENT:
		has_returning = !statements[0]->Cast<DeleteStatement>().returning_list.empty();
		break;
	default:
		break;
	}
	bool is_select_statement = (stmt_type == StatementType::SELECT_STATEMENT) || has_returning;
	auto prepared = conn->Prepare(std::move(statements[0]));
	if (!prepared || prepared->HasError()) {
		auto msg = prepared ? prepared->GetError() : "prepare failed";
		RespondError(res, 400, ClassifyError(msg), msg, parsed.session_id);
		return;
	}

	// Introspect parameter types via prepared->GetExpectedParameterTypes().
	// Positional params ($1, $2, ...) appear with keys "1", "2", ... in
	// the named map. We iterate from "1" upward to build the type vector
	// in positional order.
	vector<LogicalType> expected_types; // duckdb::vector — needed to match SqlParamDecoder::Decode signature
	try {
		auto expected_map = prepared->GetExpectedParameterTypes();
		auto n_params = expected_map.size();
		expected_types.reserve(n_params);
		for (idx_t i = 1; i <= n_params; i++) {
			auto it = expected_map.find(std::to_string(i));
			if (it == expected_map.end()) {
				// Non-positional or named parameters in the prepared
				// statement; fall back to ANY (Mode B wrapper required).
				expected_types.push_back(LogicalType::ANY);
			} else {
				expected_types.push_back(it->second);
			}
		}
	} catch (const std::exception &ex) {
		RespondError(res, 400, "BAD_REQUEST",
		              StringUtil::Format("could not introspect parameter types: %s", ex.what()),
		              parsed.session_id);
		return;
	}

	fjs::SqlParamDecoder param_decoder;
	fjs::DecodedParams params_out;
	if (!parsed.params_json.empty()) {
		params_out = param_decoder.Decode(parsed.params_json, expected_types, *conn->context);
		if (!params_out.ok) {
			RespondError(res, 400, "BAD_REQUEST", params_out.error, parsed.session_id);
			return;
		}
	} else if (!expected_types.empty()) {
		RespondError(res, 400, "BAD_REQUEST",
		              StringUtil::Format("prepared statement expects %zu parameter(s); none provided",
		                                  expected_types.size()),
		              parsed.session_id);
		return;
	}

	// PR-7b — guard the in-flight query for /sessions visibility AND
	// for harbor_query_timeout_s enforcement.
	//
	// SessionManager-tracked (non-ephemeral) sessions use
	// QueryExecutionGuard, which the timeout sweeper observes and
	// interrupts when the deadline elapses. The guard's destructor
	// (or ~StreamingCtx, for the streaming path) clears the deadline
	// + in_flight signal.
	//
	// Ephemeral /sql sessions aren't in the SessionManager pool, so
	// the sweeper can't see them; they get a per-request RAII
	// QueryTimeoutWatchdog instead, which spawns one std::thread that
	// waits on a condition_variable until either the deadline elapses
	// (call Interrupt()) or the destructor signals done (clean exit).
	uint64_t timeout_seconds = ReadQueryTimeoutSeconds(*db_locked);
	unique_ptr<QueryExecutionGuard> session_guard;
	unique_ptr<QueryTimeoutWatchdog> ephemeral_watchdog;
	if (!resolved.ephemeral && resolved.session) {
		// Caller holds session.lock (acquired earlier via try_to_lock);
		// safe to construct a QueryExecutionGuard, which mutates
		// last_query under that lock.
		session_guard = make_uniq<QueryExecutionGuard>(*resolved.session, parsed.sql, timeout_seconds);
	} else if (resolved.ephemeral && conn) {
		ephemeral_watchdog = make_uniq<QueryTimeoutWatchdog>(*conn, timeout_seconds);
	}

	// Helper closure for "did the right guard observe a timeout?" —
	// folds the two paths so the catch sites stay readable.
	auto query_timed_out = [&]() -> bool {
		if (session_guard && session_guard->TimedOut()) {
			return true;
		}
		if (ephemeral_watchdog && ephemeral_watchdog->TimedOut()) {
			return true;
		}
		return false;
	};

	auto started_at = std::chrono::steady_clock::now();
	unique_ptr<QueryResult> result;
	try {
		// Default allow_stream_result=true; passing the bool explicitly
		// trips the variadic Execute<ARGS...> overload (which tries to
		// CreateValue<vector<Value>> and fails the static_assert).
		result = prepared->Execute(params_out.values);
	} catch (const std::exception &ex) {
		if (query_timed_out()) {
			RespondError(res, 504, "QUERY_TIMEOUT",
			              "query exceeded harbor_query_timeout_s", parsed.session_id);
			return;
		}
		RespondError(res, 400, ClassifyError(ex.what()), ex.what(), parsed.session_id);
		return;
	}
	if (!result || result->HasError()) {
		auto msg = result ? result->GetError() : "execute failed";
		if (query_timed_out()) {
			RespondError(res, 504, "QUERY_TIMEOUT",
			              "query exceeded harbor_query_timeout_s", parsed.session_id);
			return;
		}
		RespondError(res, 400, ClassifyError(msg), msg, parsed.session_id);
		return;
	}

	// Choose response shape.
	auto shape = ChooseShape(req.get_header_value("Accept"));

	// Non-SELECT statements without RETURNING → one-shot write response.
	// We use statement shape rather than column count because vanilla
	// INSERT/UPDATE/DELETE return a single-column "Count" pseudo-result
	// that would otherwise look like a SELECT result of one BIGINT row.
	// DML with RETURNING is intentionally classified as select-shaped:
	// it produced real columns that clients asked to receive.
	if (!is_select_statement) {
		// Pull the affected-rows count from the result's first row if
		// available (DuckDB's INSERT/UPDATE/DELETE convention).
		idx_t affected = 0;
		try {
			auto chunk = result->Fetch();
			if (chunk && chunk->size() > 0 && chunk->ColumnCount() > 0) {
				auto v = chunk->GetValue(0, 0);
				if (!v.IsNull()) {
					affected = static_cast<idx_t>(v.GetValue<int64_t>());
				}
			}
		} catch (...) {
			affected = 0;
		}
		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		                       std::chrono::steady_clock::now() - started_at)
		                       .count();
		fjs::JsonWriter w;
		fjs::SqlChunkEncoder write_encoder({}, {});
		write_encoder.EmitOneShotWrite(w, parsed.session_id, affected, elapsed_ms);
		res.status = 200;
		res.set_content(w.Take(), "application/json");
		return;
	}

	auto encoder = make_uniq<fjs::SqlChunkEncoder>(result->names, result->types);

	// One-shot JSON path: materialize everything into a single JSON object.
	if (shape == ResponseShape::kOneShotJson) {
		auto max_rows = ReadUbigintSetting(db, "harbor_max_response_rows", 0);
		vector<unique_ptr<DataChunk>> all_chunks;
		idx_t row_count = 0;
		bool truncated = false;
		while (true) {
			unique_ptr<DataChunk> chunk;
			try {
				chunk = result->Fetch();
			} catch (const std::exception &ex) {
				if (query_timed_out()) {
					RespondError(res, 504, "QUERY_TIMEOUT",
					              "query exceeded harbor_query_timeout_s", parsed.session_id);
					return;
				}
				RespondError(res, 500, ClassifyError(ex.what()), ex.what(), parsed.session_id);
				return;
			}
			if (!chunk || chunk->size() == 0) {
				break;
			}
			row_count += chunk->size();
			if (max_rows > 0 && row_count >= max_rows) {
				// Trim this chunk to the cap.
				if (row_count > max_rows) {
					auto excess = row_count - max_rows;
					chunk->SetCardinality(chunk->size() - excess);
					row_count = max_rows;
				}
				all_chunks.push_back(std::move(chunk));
				truncated = true;
				break;
			}
			all_chunks.push_back(std::move(chunk));
		}
		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		                       std::chrono::steady_clock::now() - started_at)
		                       .count();
		std::vector<reference<DataChunk>> chunk_refs;
		chunk_refs.reserve(all_chunks.size());
		for (auto &uptr : all_chunks) {
			chunk_refs.emplace_back(*uptr);
		}
		fjs::JsonWriter w;
		encoder->EmitOneShot(w, parsed.session_id, "select", chunk_refs, row_count, elapsed_ms);
		// Note: truncation flag not in one-shot envelope per SPEC §5.2;
		// only NDJSON end record carries it. Acceptable trade-off: clients
		// that care about truncation use NDJSON. (Non-blocking enhancement
		// for PR-7 hardening.)
		(void)truncated;
		res.status = 200;
		res.set_content(w.Take(), "application/json");
		return;
	}

	// NDJSON streaming path. Build a shared StreamingCtx so the closure
	// outlives the route lambda. ActiveRequestGuard + session_lock both
	// MOVED into the ctx so they live for the full streaming duration.
	auto ctx = std::make_shared<StreamingCtx>();
	ctx->guard = std::make_shared<HarborHttpServer::ActiveRequestGuard>(server);
	ctx->session = resolved.ephemeral ? nullptr : resolved.session;
	ctx->ephemeral_connection = std::move(ephemeral_conn);
	ctx->session_lock = std::move(session_lock);
	ctx->result = std::move(result);
	ctx->encoder = std::move(encoder);
	ctx->session_id_for_response = parsed.session_id;
	ctx->started_at = started_at;
	ctx->max_rows = ReadUbigintSetting(db, "harbor_max_response_rows", 0);
	ctx->chunk_mode = (shape == ResponseShape::kNdjsonChunkMode);
	// PR-7b — transfer the timeout guard / watchdog ownership to ctx
	// so its destructor (running after the streaming provider's last
	// invocation, or upon premature shutdown) does the cleanup
	// (sessionful: clear deadline + in_flight; ephemeral: signal
	// done + join the watchdog thread). The local unique_ptrs become
	// empty for the streaming path; non-streaming returns above us
	// already destructed their guards via stack unwinding.
	ctx->session_guard = std::move(session_guard);
	ctx->ephemeral_watchdog = std::move(ephemeral_watchdog);

	res.status = 200;
	res.set_chunked_content_provider("application/x-ndjson", [ctx](size_t /*offset*/,
	                                                                duckdb_httplib_openssl::DataSink &sink) -> bool {
		try {
		// Phase 1: schema (first call).
		if (!ctx->schema_emitted) {
			fjs::JsonWriter w;
			ctx->encoder->EmitSchema(w, ctx->session_id_for_response);
			if (!WriteOneLine(sink, w)) {
				return false;
			}
			ctx->schema_emitted = true;
			return true;
		}
		// Phase 2: rows / chunks. Each provider call emits ONE chunk.
		if (!ctx->end_emitted && !ctx->error_emitted) {
			unique_ptr<DataChunk> chunk;
			try {
				chunk = ctx->result->Fetch();
			} catch (const std::exception &ex) {
				// Mid-stream error: emit error line directly in the
				// catch (round-15 GPT-5.5: don't rely on httplib
				// calling us back after returning true).
				// PR-7b (round 22) — classify by interrupt cause so
				// QUERY_TIMEOUT, QUERY_CANCELLED, and generic SQL
				// errors are distinguishable to the client. Cause()
				// returns NONE when no interrupt fired (generic
				// SQL exception path).
				auto cause = ctx->Cause();
				if (cause == InterruptCause::TIMEOUT) {
					EmitStreamingErrorSafe(*ctx, sink, "QUERY_TIMEOUT",
					                        "query exceeded harbor_query_timeout_s");
					return false;
				}
				if (cause == InterruptCause::USER_CANCEL) {
					EmitStreamingErrorSafe(*ctx, sink, "QUERY_CANCELLED",
					                        "query was cancelled by /sql/cancel or /interrupt");
					return false;
				}
				EmitStreamingErrorSafe(*ctx, sink, ClassifyError(ex.what()), ex.what());
				return false;
			}
			if (!chunk || chunk->size() == 0) {
				// PR-7b (round 22 fix) — DuckDB's
				// StreamingQueryResult returns empty (not an
				// exception) when Connection::Interrupt fires
				// mid-stream, regardless of WHO fired it. So an
				// empty chunk could mean (a) genuine end of result,
				// (b) timeout sweeper / watchdog interrupted, OR
				// (c) /sql/cancel + /interrupt user-cancellation.
				// Without classifying these distinctly, an
				// interrupted stream would emit `{"type":"end"}` and
				// the client would think the query succeeded with
				// fewer rows — exactly the round-22 GPT-5.5 catch.
				auto cause = ctx->Cause();
				if (cause == InterruptCause::TIMEOUT) {
					EmitStreamingErrorSafe(*ctx, sink, "QUERY_TIMEOUT",
					                        "query exceeded harbor_query_timeout_s");
					return false;
				}
				if (cause == InterruptCause::USER_CANCEL) {
					EmitStreamingErrorSafe(*ctx, sink, "QUERY_CANCELLED",
					                        "query was cancelled by /sql/cancel or /interrupt");
					return false;
				}
				if (cause == InterruptCause::DISCONNECT) {
					// Client gone; close cleanly without an error
					// frame the caller wouldn't read anyway. Today
					// the DISCONNECT path doesn't actually call
					// Interrupt(), so this branch is forward-
					// compatible groundwork for the post-v0.1 wiring.
					sink.done();
					return false;
				}
				// End of result.
				auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				                       std::chrono::steady_clock::now() - ctx->started_at)
				                       .count();
				fjs::JsonWriter w;
				ctx->encoder->EmitEnd(w, ctx->row_count, elapsed_ms, ctx->truncated);
				WriteOneLine(sink, w);
				sink.done();
				ctx->end_emitted = true;
				return false;
			}
			// Apply row cap if configured.
			idx_t emit_size = chunk->size();
			if (ctx->max_rows > 0 && ctx->row_count + emit_size > ctx->max_rows) {
				emit_size = ctx->max_rows - ctx->row_count;
				chunk->SetCardinality(emit_size);
				ctx->truncated = true;
			}
			// Build the entire payload for THIS chunk into one buffer,
			// then sink.write() once. Buffer-before-write discipline:
			// if EmitRow/EmitChunk/GetValue throws partway through,
			// the half-built buffer is discarded and the OUTER catch
			// below emits a clean NDJSON error line before closing.
			std::string out_buf;
			if (ctx->chunk_mode) {
				fjs::JsonWriter w;
				ctx->encoder->EmitChunk(w, *chunk);
				out_buf = w.Take();
				out_buf.push_back('\n');
			} else {
				for (idx_t row = 0; row < chunk->size(); row++) {
					fjs::JsonWriter row_w;
					ctx->encoder->EmitRow(row_w, *chunk, row);
					out_buf.append(row_w.Take());
					out_buf.push_back('\n');
				}
			}
			if (!sink.write(out_buf.data(), out_buf.size())) {
				// Client disconnected; stop streaming.
				return false;
			}
			ctx->row_count += emit_size;
			if (ctx->truncated) {
				auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				                       std::chrono::steady_clock::now() - ctx->started_at)
				                       .count();
				fjs::JsonWriter w_end;
				ctx->encoder->EmitEnd(w_end, ctx->row_count, elapsed_ms, true);
				WriteOneLine(sink, w_end);
				sink.done();
				ctx->end_emitted = true;
				return false;
			}
			return true;
		}
		// Already emitted end or error.
		sink.done();
		return false;
		} catch (const std::exception &ex) {
			// Covers encoder/JsonWriter/Value extraction failures in
			// addition to result->Fetch() failures. Round-16 catch:
			// the fetch-only catch above was insufficient because
			// EmitRow/EmitChunk can throw after a chunk is fetched.
			// PR-7b (round 22) — classify by interrupt cause as in
			// the inner catch above: QUERY_TIMEOUT, QUERY_CANCELLED,
			// or fall through to ClassifyError for non-interrupt
			// exceptions (encoder bugs, etc.).
			auto cause = ctx->Cause();
			if (cause == InterruptCause::TIMEOUT) {
				EmitStreamingErrorSafe(*ctx, sink, "QUERY_TIMEOUT",
				                        "query exceeded harbor_query_timeout_s");
				return false;
			}
			if (cause == InterruptCause::USER_CANCEL) {
				EmitStreamingErrorSafe(*ctx, sink, "QUERY_CANCELLED",
				                        "query was cancelled by /sql/cancel or /interrupt");
				return false;
			}
			EmitStreamingErrorSafe(*ctx, sink, ClassifyError(ex.what()), ex.what());
			return false;
		}
	});
}

// ---------- POST /sql/sessions/new ----------

void SqlHandlers::HandleSessionNew(const duckdb_httplib_openssl::Request &req,
                                     duckdb_httplib_openssl::Response &res) {
	HarborHttpServer::ActiveRequestGuard guard(server);
	ApplyCors(auth, req, res);

	auto authn = AuthenticateForSql(auth, req, kAdminSessionId);
	if (!authn.ok) {
		RespondError(res, authn.status, authn.error_code, authn.message);
		return;
	}
	if (!auth.RunAuthorization(kAdminSessionId, kAuthzCreateSession)) {
		RespondError(res, 403, "FORBIDDEN", "authorization callback rejected session creation");
		return;
	}
	try {
		auto sid = sessions.GenerateSessionId();
		sessions.CreateOwnedSession(sid, authn.principal_id);
		fjs::JsonWriter w;
		w.BeginObject();
		w.KeyBool("ok", true);
		w.KeyString("sessionId", sid);
		w.EndObject();
		res.status = 200;
		res.set_content(w.Take(), "application/json");
	} catch (const std::exception &ex) {
		auto code = ClassifyError(ex.what());
		auto status = code == "SESSION_LIMIT" ? 429 : 500;
		RespondError(res, status, code, ex.what());
	}
}

// ---------- DELETE /sql/sessions/:id ----------

void SqlHandlers::HandleSessionDelete(const duckdb_httplib_openssl::Request &req,
                                        duckdb_httplib_openssl::Response &res) {
	HarborHttpServer::ActiveRequestGuard guard(server);
	ApplyCors(auth, req, res);

	auto authn = AuthenticateForSql(auth, req, kAdminSessionId);
	if (!authn.ok) {
		RespondError(res, authn.status, authn.error_code, authn.message);
		return;
	}
	if (!auth.RunAuthorization(kAdminSessionId, kAuthzDeleteSession)) {
		RespondError(res, 403, "FORBIDDEN", "authorization callback rejected session deletion");
		return;
	}
	// Captured by the regex as match group 1. The Match::operator[]
	// is non-const, so we have to const_cast — the underlying
	// container is logically read-only on our side, this is a
	// cpp-httplib API quirk.
	auto &mutable_matches = const_cast<duckdb_httplib_openssl::Match &>(req.matches);
	string sid = mutable_matches[1].str();
	if (sid.empty()) {
		RespondError(res, 404, "SESSION_NOT_FOUND", "missing session id in path");
		return;
	}
	auto destroyed = sessions.DestroyOwnedSession(sid, authn.principal_id);
	if (!destroyed) {
		// Don't leak whether the session existed but was owned by
		// someone else, vs not existed at all (SPEC §6 anti-enum).
		RespondError(res, 404, "SESSION_NOT_FOUND", "no such session for this principal", sid);
		return;
	}
	fjs::JsonWriter w;
	w.BeginObject();
	w.KeyBool("ok", true);
	w.KeyString("sessionId", sid);
	w.EndObject();
	res.status = 200;
	res.set_content(w.Take(), "application/json");
}

// ---------- POST /sql/cancel (PR-6) ----------
//
// Body: {"sessionId": "<sid>"}. Admin authz: __HARBOR_ADMIN__:sessions:cancel.
// Same Connection::Interrupt() shape as AdminHandlers::HandleInterrupt
// but a separate route + authz string so policies can grant cancel
// without granting full interrupt. Cookie-authenticated callers must
// pass an Origin/Referer in the harbor_cors_origins allow-list.

void SqlHandlers::HandleSqlCancel(const duckdb_httplib_openssl::Request &req,
                                    duckdb_httplib_openssl::Response &res,
                                    const duckdb_httplib_openssl::ContentReader &content_reader) {
	HarborHttpServer::ActiveRequestGuard guard(server);
	ApplyCors(auth, req, res);

	auto authn = AuthenticateForSql(auth, req, kAdminSessionId);
	if (!authn.ok) {
		RespondError(res, authn.status, authn.error_code, authn.message);
		return;
	}
	if (authn.source == AuthSource::kCookie && !HasAllowedBrowserOrigin(server, auth, req)) {
		RespondError(res, 403, "FORBIDDEN",
		              "cookie-authenticated /sql/cancel requires an allowed Origin or Referer");
		return;
	}
	if (!auth.RunAuthorization(kAdminSessionId, kAuthzCancelSession)) {
		RespondError(res, 403, "FORBIDDEN", "authorization callback rejected the admin call");
		return;
	}
	auto ct = req.get_header_value("Content-Type");
	auto ct_lower = StringUtil::Lower(ct);
	// PR-6 follow-up (round 19/20): tighter Content-Type check.
	// Round 19 caught: the prior prefix-only `find(...) != 0` accepted
	// `application/jsonjunk`. Round 20: only `;` is a standard MIME
	// parameter separator, drop the non-standard trailing-space branch.
	while (!ct_lower.empty() && std::isspace(static_cast<unsigned char>(ct_lower.front()))) {
		ct_lower.erase(ct_lower.begin());
	}
	while (!ct_lower.empty() && std::isspace(static_cast<unsigned char>(ct_lower.back()))) {
		ct_lower.pop_back();
	}
	const bool ct_ok = ct_lower == "application/json" || StringUtil::StartsWith(ct_lower, "application/json;");
	if (!ct_ok) {
		RespondError(res, 415, "UNSUPPORTED_MEDIA_TYPE", "/sql/cancel expects Content-Type: application/json");
		return;
	}
	auto max_body = ReadUbigintSetting(db, "harbor_max_request_body_bytes", kDefaultMaxBodyBytes);
	string body;
	bool too_big = false;
	content_reader([&](const char *data, size_t length) {
		if (too_big) {
			return false;
		}
		if (body.size() + length > max_body) {
			too_big = true;
			return false;
		}
		body.append(data, length);
		return true;
	});
	if (too_big) {
		RespondError(res, 413, "PAYLOAD_TOO_LARGE", "request body exceeds harbor_max_request_body_bytes");
		return;
	}

	// Lightweight extraction — same shape as AdminHandlers'
	// ExtractJsonSessionId. Anchoring to "sessionId":"…" avoids pulling
	// in a JSON parser dep here.
	string session_id;
	auto pos = body.find("\"sessionId\"");
	if (pos != string::npos) {
		auto colon = body.find(':', pos);
		if (colon != string::npos) {
			auto q1 = body.find('"', colon);
			if (q1 != string::npos) {
				auto q2 = body.find('"', q1 + 1);
				if (q2 != string::npos) {
					session_id = body.substr(q1 + 1, q2 - (q1 + 1));
				}
			}
		}
	}
	if (session_id.empty()) {
		RespondError(res, 400, "BAD_REQUEST", "expected JSON body with a non-empty 'sessionId' field");
		return;
	}

	if (!sessions.InterruptSession(session_id)) {
		RespondError(res, 404, "SESSION_NOT_FOUND", "no session with the given id");
		return;
	}
	fjs::JsonWriter w;
	w.BeginObject();
	w.KeyBool("ok", true);
	w.KeyString("sessionId", session_id);
	w.EndObject();
	res.status = 200;
	res.set_content(w.Take(), "application/json");
}

// ---------- registration ----------

void SqlHandlers::Register(duckdb_httplib_openssl::Server &http) {
	auto *self = this;
	http.Post("/sql", [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res,
	                         const duckdb_httplib_openssl::ContentReader &content_reader) {
		self->HandleSql(req, res, content_reader);
	});
	http.Post("/sql/sessions/new",
	          [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
		          self->HandleSessionNew(req, res);
	          });
	// PR-6: /sql/cancel is registered BEFORE the /sql/sessions/:id
	// regex below so the more-specific path wins.
	http.Post("/sql/cancel",
	          [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res,
	                 const duckdb_httplib_openssl::ContentReader &content_reader) {
		          self->HandleSqlCancel(req, res, content_reader);
	          });
	// cpp-httplib's `:id` syntax exists in PathParamsMatcher but
	// Server::Delete (and Get/Post) wire patterns directly through
	// Regex() — bypassing make_matcher() — so `:id` is interpreted as
	// a regex literal and never matches. Use an explicit regex
	// capture; the handler reads req.matches[1].str() (which is safe
	// without a size check because the handler only runs when the
	// regex matches).
	http.Delete(R"(^/sql/sessions/([^/]+)$)",
	            [self](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
		            self->HandleSessionDelete(req, res);
	            });
}

} // namespace duckdb
