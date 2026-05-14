#include "admin_handlers.hpp"

#include "flock_http_server.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"

#include <chrono>

namespace duckdb {

namespace {

// flock version. Compiled in from EXT_VERSION_FLOCK if the build
// system provides it (DuckDB's build system auto-defines this from
// the extension name); otherwise "unknown" so /health is always
// answerable.
const char *FlockVersion() {
#ifdef EXT_VERSION_FLOCK
	return EXT_VERSION_FLOCK;
#else
	return "unknown";
#endif
}

// Quack protocol version. Constant for the lifetime of v1.5.x quack;
// when we rebase to a future quack release with a new wire-format
// version, bump this.
constexpr const char *kQuackProtocolVersion = "1";

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
			out += c;
		}
	}
	return out;
}

} // namespace

AdminHandlers::AdminHandlers(FlockHttpServer &server_p) : server(server_p) {
}

void AdminHandlers::Register(duckdb_httplib_openssl::Server &http) {
	auto *self = this;

	// GET /health — public, four fields exactly per SPEC §5.5.
	// No DB info, no path, no token, no extension list, no bind
	// address, no auth principal — anything else risks information
	// disclosure on a remote-bound deploy.
	http.Get("/health", [self](const duckdb_httplib_openssl::Request &, duckdb_httplib_openssl::Response &res) {
		FlockHttpServer::ActiveRequestGuard guard(self->server);

		auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
		                    std::chrono::steady_clock::now() - self->server.StartedAt())
		                    .count();
		auto body = StringUtil::Format("{\"ok\":true,\"version\":\"%s\",\"uptime_s\":%lld}",
		                               EscapeJsonString(FlockVersion()), static_cast<long long>(uptime_s));
		res.set_content(body, "application/json");
	});

	// GET /info — public; empty body, version metadata in headers so
	// the DuckDB UI can detect the server without running a query.
	// SPEC §11 lists the headers; PR-3 will add X-DuckDB-UI-Extension-Version
	// once UiHandlers lands so the official UI can detect us.
	http.Get("/info", [self](const duckdb_httplib_openssl::Request &, duckdb_httplib_openssl::Response &res) {
		FlockHttpServer::ActiveRequestGuard guard(self->server);

		res.set_header("X-Flock-Version", FlockVersion());
		res.set_header("X-DuckDB-Version", DuckDB::LibraryVersion());
		res.set_header("X-DuckDB-Platform", DuckDB::Platform());
		res.set_header("X-Quack-Protocol-Version", kQuackProtocolVersion);
		res.status = 204;
	});
}

} // namespace duckdb
