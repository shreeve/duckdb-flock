// PR-1 transitional fork — see AGENTS.md "Implementation roadmap"
// and docs/upstream-quack-patches.md for the rebase process.
//
// This file is duckdb-quack's quack_extension.cpp at v1.5-variegata
// (commit 90bd70e), with five minimal harbor-specific edits:
//
//   1. The DUCKDB_CPP_EXTENSION_ENTRY symbol is renamed from `quack` to
//      `harbor` so DuckDB locates the right entry point when loading
//      harbor.duckdb_extension.
//   2. The reported version macro is `EXT_VERSION_HARBOR` (DuckDB's build
//      system auto-defines this from the extension name) instead of
//      `EXT_VERSION_RPC` (which corresponded to upstream's EXT_NAME=rpc).
//   3. A new `harbor_version()` scalar function is registered alongside
//      the upstream `quack_*` SQL surface, so a smoke test can verify
//      the extension loaded and exports harbor-named identifiers.
//   4. `QuackExtension::Name()` returns "harbor" so the C++ Extension
//      class identity matches the loadable extension name. The class
//      is still spelled QuackExtension to keep the diff minimal; only
//      its reported Name() changes.
//   5. `loader.SetDescription(...)` text reflects harbor's purpose
//      (visible in `duckdb_extensions()` listing).
//
// The upstream Quack wire format and Quack-compatible SQL surface are
// preserved unchanged (`quack_*`, `quack:`, `/quack`). Harbor-owned
// lifecycle/settings/auth names are registered alongside that compatibility
// layer. The architectural refactor that extracts httplib::Server from
// QuackServer into a shared HarborHttpServer lands in PR-2.

#define DUCKDB_EXTENSION_MAIN

#include "duckdb/catalog/default/default_table_functions.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "storage/quack_optimizer.hpp"

#include "include/storage/quack_catalog.hpp"
#include "quack_clear_cache.hpp"
#include "quack_extension.hpp"
#include "quack_log.hpp"
#include "quack_scan.hpp"
#include "quack_startstop.hpp"
#include "quack_storage.hpp"
#include "quack_uri.hpp"
#include "include/quack_startstop.hpp"
#include "include/quack_storage.hpp"
#include "include/quack_uri.hpp"

// PR-2: harbor-original lifecycle functions registered alongside quack_*.
#include "harbor_lifecycle.hpp"

// PR-3: UI extension state + settings registration.
#include "settings.hpp"     // UI_LOCAL_PORT_SETTING_NAME et al.
#include "state.hpp"        // UIStorageExtensionInfo, STORAGE_EXTENSION_KEY = "ui"
#include "utils/env.hpp"    // GetEnvOrDefault[Int]

namespace duckdb {

static constexpr const char *QUACK_SECRET_TYPE = "quack";

static unique_ptr<BaseSecret> CreateQuackSecretFromConfig(ClientContext &, CreateSecretInput &input) {
	auto scope = input.scope;
	if (scope.empty()) {
		scope.emplace_back("quack:");
	}
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	for (const auto &named_param : input.options) {
		auto lower_name = StringUtil::Lower(named_param.first);
		if (lower_name == "token") {
			secret->secret_map["token"] = named_param.second.ToString();
		} else {
			throw InvalidInputException("Unknown named parameter for quack secret: %s", lower_name);
		}
	}
	secret->redact_keys = {"token"};
	return std::move(secret);
}

static void RegisterQuackSecretType(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = QUACK_SECRET_TYPE;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	secret_type.extension = "quack";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction config_fun = {QUACK_SECRET_TYPE, "config", CreateQuackSecretFromConfig};
	config_fun.named_parameters["token"] = LogicalType::VARCHAR;
	loader.RegisterFunction(config_fun);
}

static bool TimingSafeEqual(const string &a, const string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	volatile unsigned char result = 0;
	for (size_t i = 0; i < a.size(); i++) {
		result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
	}
	return result == 0;
}

// pass session id
static void QuackAuthToken(const DataChunk &args, ExpressionState &state, Vector &result) {
	auto client_token = args.GetValue(1, 0).GetValue<string>();
	auto server_token = args.GetValue(2, 0).GetValue<string>();

	result.SetValue(0, Value::BOOLEAN(TimingSafeEqual(client_token, server_token)));
}

static void QuackDummyAuthorization(const DataChunk &args, ExpressionState &, Vector &result) {
	result.SetValue(0, Value(true)); // choose life
}

// harbor-specific: returns the build's extension version string. Lets a
// smoke test confirm the extension loaded and that harbor-named symbols
// are reachable, without depending on any upstream quack surface.
//
// Implemented as a constant-vector reference so the result is correct
// regardless of cardinality (e.g. `SELECT harbor_version() FROM range(10)`).
static void HarborVersionScalar(const DataChunk &, ExpressionState &, Vector &result) {
#ifdef EXT_VERSION_HARBOR
	Value version(EXT_VERSION_HARBOR);
#else
	Value version("unknown");
#endif
	result.Reference(version);
}

static void QuackIdentifyFun(ClientContext &, TableFunctionInput &, DataChunk &) {
	// No-op: side effects are in bind.
}

static unique_ptr<FunctionData> QuackIdentifyBind(ClientContext &ctx, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto &db_config = DBConfig::GetConfig(ctx);
	for (auto &kv : input.named_parameters) {
		if (kv.second.IsNull()) {
			continue;
		}
		db_config.SetOptionByName("whoami_" + kv.first, kv.second);
	}
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("ok");
	return nullptr;
}

static TableFunction GetQuackIdentifyFunction() {
	TableFunction fun("quack_identify", {}, QuackIdentifyFun, QuackIdentifyBind);
	fun.named_parameters["name"] = LogicalType::VARCHAR;
	fun.named_parameters["provider"] = LogicalType::VARCHAR;
	fun.named_parameters["hostname"] = LogicalType::VARCHAR;
	fun.named_parameters["region"] = LogicalType::VARCHAR;
	fun.named_parameters["meta"] = LogicalType::VARCHAR; // JSON as string
	return fun;
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("harbor — DuckDB as an HTTP service (PR-2: HarborHttpServer)");

	// Auto-load httpfs. harbor_serve uses DuckDB's writable crypto module
	// (HMAC-SHA256 cookie signing, CSPRNG cookie-nonce generation,
	// CSP-nonce generation) and that module is provided by the libcrypto
	// link inside the httpfs extension. Without httpfs loaded first,
	// harbor_serve fails with:
	//
	//   Invalid Configuration Error: DuckDB currently has a read-only
	//   crypto module loaded. Please ensure httpfs is loaded using
	//   `LOAD httpfs`...
	//
	// The community-installed harbor (`INSTALL harbor FROM community`)
	// auto-pulls httpfs as a transitive dep via DuckDB's package manifest;
	// side-loaded local builds (downloaded from the GitHub Release, or
	// `LOAD '/abs/path/harbor.duckdb_extension'`) don't, so historically
	// users had to remember `LOAD httpfs;` first. This auto-load
	// closes that footgun for both install paths.
	//
	// Failures here are intentionally swallowed — if AutoLoadExtension
	// can't find httpfs (custom DuckDB build, network-isolated env, etc.),
	// the operator will still get DuckDB's clearer error from harbor_serve
	// itself, which already explains the fix.
	try {
		ExtensionHelper::AutoLoadExtension(loader.GetDatabaseInstance(), "httpfs");
	} catch (...) {
		// Best-effort; harbor_serve will still surface a clear error.
	}

	// PR-3 follow-up (vendored from upstream `duckdb-ui` LoadInternal,
	// `misc/duckdb-ui/src/ui_extension.cpp` ll. 105-107): ensure
	// `~/.duckdb/extension_data/ui/` exists. The DuckDB UI's JavaScript
	// bundle expects this directory at startup so it can ATTACH
	// `<path>/ui.db AS _duckdb_ui` for its own state (notebooks, query
	// history, settings). DuckDB's ATTACH creates the file on first use,
	// but it does NOT create missing parent directories, so a fresh
	// machine — or one whose `~/.duckdb` was wiped — would fail with
	// `Initialization Error: Catalog "_duckdb_ui" does not exist`. This
	// piece of upstream's `LoadInternal` was missed when we vendored the
	// UI server portion in PR-3 (see docs/upstream-ui-patches.md); it
	// surfaced when a user wiped `~/.duckdb` to test v0.2.0 and got the
	// "Failed to resolve app state with user" modal.
	try {
		auto &fs = FileSystem::GetFileSystem(loader.GetDatabaseInstance());
		fs.CreateDirectory(fs.ExpandPath("~/.duckdb/extension_data"));
		fs.CreateDirectory(fs.ExpandPath("~/.duckdb/extension_data/ui"));
	} catch (...) {
		// Best-effort. If the OS denies the create (read-only home dir,
		// non-existent home, sandbox), the UI-init path will still
		// surface a clearer error to the operator.
	}

	// harbor-specific: SELECT harbor_version() as the smoke-test surface.
	// Not volatile — the build's version string is deterministic for the
	// lifetime of the process.
	ScalarFunction harbor_version_fun("harbor_version", {}, LogicalType::VARCHAR, HarborVersionScalar);
	loader.RegisterFunction(harbor_version_fun);

	// Vendored quack table functions. quack_serve / quack_stop are now
	// thin shims that delegate to HarborServerState::Global() (see
	// src/quack/quack_start_stop.cpp).
	loader.RegisterFunction(QuackScanFunction::GetFunction());
	loader.RegisterFunction(QuackScanByNameFunction::GetFunction());
	loader.RegisterFunction(QuackServeFunction::GetFunction());
	loader.RegisterFunction(QuackStopFunction::GetFunction());
	loader.RegisterFunction(QuackServerListFunction::GetFunction());
	loader.RegisterFunction(QuackClearCacheFunction::GetFunction());
	loader.RegisterFunction(GetQuackIdentifyFunction());

	// harbor-named lifecycle functions per SPEC §9. Same underlying
	// HarborServerState as quack_serve/stop above, but a different SQL
	// surface — harbor_serve uses "harbor:" defaults, harbor_wait blocks
	// for daemon-mode init scripts (no quack_wait alias because
	// upstream never had one).
	loader.RegisterFunction(HarborServeFunction::GetFunction());
	loader.RegisterFunction(HarborStopFunction::GetFunction());
	loader.RegisterFunction(HarborWaitFunction::GetFunction());

	// the default authentication function
	ScalarFunction quack_check_token("quack_check_token",
	                                 {/* session id */ LogicalType::VARCHAR, /* auth string */ LogicalType::VARCHAR,
	                                  /* token */ LogicalType::VARCHAR},
	                                 LogicalType::BOOLEAN, QuackAuthToken);
	quack_check_token.SetVolatile();
	loader.RegisterFunction(quack_check_token);

	ScalarFunction harbor_check_token("harbor_check_token",
	                                  {/* session id */ LogicalType::VARCHAR, /* auth string */ LogicalType::VARCHAR,
	                                   /* token */ LogicalType::VARCHAR},
	                                  LogicalType::BOOLEAN, QuackAuthToken);
	harbor_check_token.SetVolatile();
	loader.RegisterFunction(harbor_check_token);

	ScalarFunction rpc_authorization("quack_nop_authorization",
	                                 {/* session id */ LogicalType::VARCHAR, /* query string */ LogicalType::VARCHAR},
	                                 LogicalType::BOOLEAN, QuackDummyAuthorization);
	rpc_authorization.SetVolatile();
	loader.RegisterFunction(rpc_authorization);

	ScalarFunction harbor_authorization("harbor_nop_authorization",
	                                    {/* session id */ LogicalType::VARCHAR, /* query string */ LogicalType::VARCHAR},
	                                    LogicalType::BOOLEAN, QuackDummyAuthorization);
	harbor_authorization.SetVolatile();
	loader.RegisterFunction(harbor_authorization);

	loader.RegisterFunction(QuackParseUriFunction::GetFunction());
	loader.RegisterFunction(QuackParseUriFunction::GetHarborFunction());

	RegisterQuackSecretType(loader);

	loader.GetDatabaseInstance().GetLogManager().RegisterLogType(make_uniq<QuackLogType>());

	// Register the QuackStorageExtension under the "quack" key so
	// `ATTACH 'quack:host'` works for stock-quack clients. PR-2: the
	// storage_info no longer carries state (server lifecycle moved to
	// HarborServerState::Global()), but the registration shape is
	// unchanged for compatibility.
	auto ext = duckdb::make_shared_ptr<QuackStorageExtension>();
	ext->storage_info = duckdb::make_uniq<QuackStorageExtensionInfo>();
	StorageExtension::Register(loader.GetDatabaseInstance().config, QuackStorageExtensionInfo::STORAGE_EXTENSION_KEY,
	                           ext);

	auto harbor_ext = duckdb::make_shared_ptr<QuackStorageExtension>();
	harbor_ext->storage_info = duckdb::make_uniq<QuackStorageExtensionInfo>();
	StorageExtension::Register(loader.GetDatabaseInstance().config,
	                           QuackStorageExtensionInfo::HARBOR_STORAGE_EXTENSION_KEY, harbor_ext);

	// PR-3: Register the UI StorageExtension under the "ui" key so
	// UiHandlers can look up its per-tab connection pool via
	// UIStorageExtensionInfo::GetState(*db). The storage_info IS
	// stateful here (it holds the connections map); separate concept
	// from quack's SessionManager. Bare StorageExtension (no attach
	// callback) — the ATTACH path isn't relevant for UI; this is just
	// state attached to a key.
	auto ui_ext = duckdb::make_shared_ptr<StorageExtension>();
	ui_ext->storage_info = duckdb::make_uniq<UIStorageExtensionInfo>();
	StorageExtension::Register(loader.GetDatabaseInstance().config, STORAGE_EXTENSION_KEY, ui_ext);

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("harbor_authentication_function", "Name of a Harbor callback function for authentication",
	                          LogicalType::VARCHAR, Value(""), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("harbor_authorization_function", "Name of a Harbor callback function for authorization",
	                          LogicalType::VARCHAR, Value(""), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("quack_authentication_function", "Name of a Quack-compatible callback function for authentication",
	                          LogicalType::VARCHAR, Value(""), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("quack_authorization_function", "Name of a Quack-compatible callback function for authorization",
	                          LogicalType::VARCHAR, Value(""), nullptr, SetScope::GLOBAL);

	config.AddExtensionOption("quack_fetch_batch_chunks", "Maximum number of DataChunks returned per FETCH response",
	                          LogicalType::UBIGINT, Value::UBIGINT(12));

	// PR-4: cookie auth + CORS allow-list settings (per SPEC §9). The
	// cookie SIGNING KEY is deliberately NOT a SQL-readable setting in
	// v0.1 — exposing the HMAC secret to authorized SQL would let any
	// SQL caller mint cookies. AuthManager generates 32 random bytes
	// per process at first use; v0.2 reintroduces operator control
	// via the HARBOR_COOKIE_SIGNING_KEY environment variable.
	config.AddExtensionOption("harbor_auth_cookie_ttl_s",
	                          "TTL in seconds for HMAC-signed harbor_session cookies issued by /auth/login (default 12h)",
	                          LogicalType::UBIGINT, Value::UBIGINT(43200), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("harbor_cors_origins",
	                          "Comma-separated allow-list of origins for cross-origin /quack, /auth/*, /sql, /info "
	                          "(empty = no cross-origin permitted; '*' is rejected)",
	                          LogicalType::VARCHAR, Value(""), nullptr, SetScope::GLOBAL);
	// v0.2: harbor_local_dev_mode was removed. Setting it now raises a
	// hard error pointing at the replacement. We deliberately reject any
	// attempt — including `SET GLOBAL harbor_local_dev_mode = false` —
	// so stale configs surface loudly instead of silently doing nothing.
	// The replacement (`harbor_serve(uri, token := NULL)`) covers the
	// same use case AND opens auth bypass to /sql + /quack uniformly,
	// not just the UI surface (which was the v0.1 asymmetry footgun).
	config.AddExtensionOption(
	    "harbor_local_dev_mode",
	    "REMOVED in v0.2. Use harbor_serve(uri, token := NULL) on a loopback bind for unauthenticated mode.",
	    LogicalType::BOOLEAN, Value::BOOLEAN(false),
	    [](ClientContext &, SetScope, Value &) {
		    throw InvalidInputException(
		        "harbor_local_dev_mode was removed in harbor v0.2. To run an unauthenticated harbor on a loopback "
		        "bind, pass NULL for the token instead:\n"
		        "    CALL harbor_serve('harbor:127.0.0.1:9494', token := NULL);\n"
		        "Unlike the v0.1 setting (which only relaxed auth on the UI surface), token := NULL applies "
		        "uniformly to /sql, /quack, /ddb/*, and /localEvents.");
	    },
	    SetScope::GLOBAL);

	// PR-5: /sql endpoint limits per SPEC §6.
	config.AddExtensionOption("harbor_max_sessions",
	                          "Maximum concurrent DB sessions across all principals; new session creation past "
	                          "this limit returns 429 SESSION_LIMIT (default 1024)",
	                          LogicalType::UBIGINT, Value::UBIGINT(1024), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("harbor_max_response_rows",
	                          "Cap on rows returned per /sql request; 0 = unlimited; truncation reflected in the "
	                          "NDJSON end record's truncated:true field (default 0)",
	                          LogicalType::UBIGINT, Value::UBIGINT(0), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption(
	    "harbor_max_request_body_bytes",
	    "Maximum POST body size for /sql JSON requests; larger requests return 413 PAYLOAD_TOO_LARGE "
	    "(default 256 MiB; matches the nginx/Caddy reverse-proxy guidance for /quack APPEND payloads)",
	    LogicalType::UBIGINT, Value::UBIGINT(268435456ULL), nullptr, SetScope::GLOBAL);

	// PR-6: admin endpoints (/whoami, /tables, /schema/:db/:t, /checkpoint,
	// /sessions, /interrupt, /sql/cancel) are gated by an internal
	// default-deny rule on __HARBOR_ADMIN__:* synthetic authz strings
	// when no custom harbor_authorization_function (or the quack-compat
	// alias) is configured. Setting this to TRUE is the explicit
	// "trusted-network deployment" opt-in; harbor_serve emits a loud
	// WARN log on startup whenever the combination is in effect. Per
	// SPEC §7 "Admin authorization is default-deny when no hook is
	// configured" + round-18 review.
	config.AddExtensionOption(
	    "harbor_allow_admin_without_authz",
	    "When true AND no custom harbor_authorization_function is set, admin endpoints "
	    "(__HARBOR_ADMIN__:*) bypass the internal default-deny rule. Off by default; "
	    "harbor_serve logs a loud WARN at startup when the combination is in effect.",
	    LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::GLOBAL);

	// PR-7b: per-query wall-clock timeout. 0 = no limit (matches SPEC §9
	// default; preserves the long-analytical-query workload by default).
	// Non-zero value applies to every Connection::Execute path uniformly:
	// /sql (Mode A + Mode B + streaming + one-shot), /quack PREPARE/APPEND
	// /FETCH, /ddb/run, and admin transient queries (/tables, /schema,
	// /checkpoint). Enforcement is via Connection::Interrupt() on a
	// 250ms sweeper tick (SessionManager-tracked sessions) or a
	// per-request RAII watchdog with condition_variable (ephemeral /sql
	// and transient admin/UI connections). Per round-21 review with
	// GPT-5.5: HarborSession carries `query_generation` so a stale
	// sweeper interrupt can never hit the next query, and an
	// InterruptCause enum classifies the cancellation reason so
	// QUERY_TIMEOUT can be reported with HTTP 504 (or as the mid-stream
	// NDJSON `{"type":"error","code":"QUERY_TIMEOUT"}` line) instead
	// of being indistinguishable from a user-issued /sql/cancel or
	// client-disconnect interrupt.
	config.AddExtensionOption(
	    "harbor_query_timeout_s",
	    "Per-query wall-clock timeout in seconds. 0 disables the timeout. Non-zero values "
	    "interrupt any Connection::Execute (/sql, /quack, /ddb/run, admin transients) that "
	    "runs longer than the configured limit, returning HTTP 504 with errorCode "
	    "QUERY_TIMEOUT. Default 0.",
	    LogicalType::UBIGINT, Value::UBIGINT(0), nullptr, SetScope::GLOBAL);

	// PR-3: UI extension settings. Keep upstream's `ui_*` names so
	// existing duckdb-ui tooling/docs still apply. The local-port
	// setting from upstream UI is intentionally NOT registered —
	// harbor_serve takes the URI which encodes the port; ui_local_port
	// would be confusing dead state in harbor.
	{
		auto def = GetEnvOrDefault(UI_REMOTE_URL_SETTING_NAME, UI_REMOTE_URL_SETTING_DEFAULT);
		config.AddExtensionOption(UI_REMOTE_URL_SETTING_NAME,
		                          "Remote URL the UI proxies GET /.* requests to (default ui.duckdb.org)",
		                          LogicalType::VARCHAR, Value(def));
	}
	{
		auto def = GetEnvOrDefaultInt(UI_POLLING_INTERVAL_SETTING_NAME, UI_POLLING_INTERVAL_SETTING_DEFAULT);
		config.AddExtensionOption(UI_POLLING_INTERVAL_SETTING_NAME,
		                          "UI catalog watcher polling interval in milliseconds (0 disables)",
		                          LogicalType::UINTEGER, Value::UINTEGER(def));
	}

	// Process-wide fallback anchor for whoami().uptime when whoami_started_at isn't set.
	// Stored as BIGINT epoch-microseconds to stay TZ-invariant regardless of ICU state.
	config.AddExtensionOption("quack_loaded_at_us", "Epoch microseconds at extension load", LogicalType::BIGINT,
	                          Value::BIGINT(Timestamp::GetCurrentTimestamp().value));

	// whoami() identity fields — global settings so they propagate across all sessions
	// (quack_query creates fresh server-side sessions that wouldn't see per-connection state).
	config.AddExtensionOption("whoami_name", "Human-readable name for this node", LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("whoami_provider", "Deployment provider (ec2, docker, local, ...)", LogicalType::VARCHAR,
	                          Value(""));
	config.AddExtensionOption("whoami_hostname", "Network hostname / public address", LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("whoami_region", "Deployment region", LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("whoami_started_at", "Node start time (ISO-8601 TIMESTAMP)", LogicalType::VARCHAR,
	                          Value(""));
	config.AddExtensionOption("whoami_meta", "Provider-specific metadata as JSON", LogicalType::VARCHAR, Value("{}"));

	// whoami() contract — register the table macro directly via the default-table-macro
	// machinery so function resolution in the body is deferred to invocation time
	// (avoids the get_current_timestamp / core_functions chicken-and-egg).
	static const DefaultTableMacro whoami_macro = {
	    DEFAULT_SCHEMA,       "whoami", {nullptr}, // no positional parameters
	    {{nullptr, nullptr}},                      // no named parameters
	    R"SQL(SELECT
		    NULLIF(current_setting('whoami_name'), '')::VARCHAR     AS name,
		    NULLIF(current_setting('whoami_provider'), '')::VARCHAR AS provider,
		    NULLIF(current_setting('whoami_hostname'), '')::VARCHAR AS hostname,
		    NULLIF(current_setting('whoami_region'), '')::VARCHAR   AS region,
		    to_microseconds(epoch_us(current_timestamp) - COALESCE(
		      epoch_us(NULLIF(current_setting('whoami_started_at'), '')::TIMESTAMPTZ),
		      current_setting('quack_loaded_at_us')::BIGINT
		    ))                                                      AS uptime,
		    current_timestamp                           AS ts_now,
		    json_merge_patch(
		      json_object(
		        'duckdb_version', version(),
		        'platform',       (SELECT platform FROM pragma_platform())
		      ),
		      COALESCE(TRY_CAST(current_setting('whoami_meta') AS JSON), '{}'::JSON)
		    )                                           AS meta
	    )SQL",
	};
	auto whoami_info = DefaultTableFunctionGenerator::CreateTableMacroInfo(whoami_macro);
	loader.RegisterFunction(*whoami_info);

	OptimizerExtension quack_optimizer;
	quack_optimizer.optimize_function = QuackOptimizer::Optimize;
	OptimizerExtension::Register(config, std::move(quack_optimizer));
}

void QuackExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackExtension::Name() {
	// harbor-specific: the C++ Extension class identity must match the
	// loadable extension name so DuckDB's introspection / static-registry
	// bookkeeping doesn't see a mismatch. The class is still spelled
	// QuackExtension because PR-1 minimizes diff against upstream; only
	// the reported name changes.
	return "harbor";
}

std::string QuackExtension::Version() const {
#ifdef EXT_VERSION_HARBOR
	return EXT_VERSION_HARBOR;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(harbor, loader) {
	LoadInternal(loader);
}
}
