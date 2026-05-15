// PR-1 transitional fork — see AGENTS.md "Implementation roadmap"
// and docs/upstream-quack-patches.md for the rebase process.
//
// This file is duckdb-quack's quack_extension.cpp at v1.5-variegata
// (commit 90bd70e), with five minimal flock-specific edits:
//
//   1. The DUCKDB_CPP_EXTENSION_ENTRY symbol is renamed from `quack` to
//      `flock` so DuckDB locates the right entry point when loading
//      flock.duckdb_extension.
//   2. The reported version macro is `EXT_VERSION_FLOCK` (DuckDB's build
//      system auto-defines this from the extension name) instead of
//      `EXT_VERSION_RPC` (which corresponded to upstream's EXT_NAME=rpc).
//   3. A new `flock_version()` scalar function is registered alongside
//      the upstream `quack_*` SQL surface, so a smoke test can verify
//      the extension loaded and exports flock-named identifiers.
//   4. `QuackExtension::Name()` returns "flock" so the C++ Extension
//      class identity matches the loadable extension name. The class
//      is still spelled QuackExtension to keep the diff minimal; only
//      its reported Name() changes.
//   5. `loader.SetDescription(...)` text reflects flock's purpose
//      (visible in `duckdb_extensions()` listing).
//
// Everything else — SQL function/setting names, wire format,
// QuackServer's embedded httplib::Server — is preserved unchanged. The
// architectural refactor that extracts httplib::Server from QuackServer
// into a shared FlockHttpServer lands in PR-2.

#define DUCKDB_EXTENSION_MAIN

#include "duckdb/catalog/default/default_table_functions.hpp"
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

// PR-2: flock-original lifecycle functions registered alongside quack_*.
#include "flock_lifecycle.hpp"

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

// flock-specific: returns the build's extension version string. Lets a
// smoke test confirm the extension loaded and that flock-named symbols
// are reachable, without depending on any upstream quack surface.
//
// Implemented as a constant-vector reference so the result is correct
// regardless of cardinality (e.g. `SELECT flock_version() FROM range(10)`).
static void FlockVersionScalar(const DataChunk &, ExpressionState &, Vector &result) {
#ifdef EXT_VERSION_FLOCK
	Value version(EXT_VERSION_FLOCK);
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
	loader.SetDescription("flock — DuckDB as an HTTP service (PR-2: FlockHttpServer)");

	// flock-specific: SELECT flock_version() as the smoke-test surface.
	// Not volatile — the build's version string is deterministic for the
	// lifetime of the process.
	ScalarFunction flock_version_fun("flock_version", {}, LogicalType::VARCHAR, FlockVersionScalar);
	loader.RegisterFunction(flock_version_fun);

	// Vendored quack table functions. quack_serve / quack_stop are now
	// thin shims that delegate to FlockServerState::Global() (see
	// src/quack/quack_start_stop.cpp).
	loader.RegisterFunction(QuackScanFunction::GetFunction());
	loader.RegisterFunction(QuackScanByNameFunction::GetFunction());
	loader.RegisterFunction(QuackServeFunction::GetFunction());
	loader.RegisterFunction(QuackStopFunction::GetFunction());
	loader.RegisterFunction(QuackServerListFunction::GetFunction());
	loader.RegisterFunction(QuackClearCacheFunction::GetFunction());
	loader.RegisterFunction(GetQuackIdentifyFunction());

	// flock-named lifecycle functions per SPEC §9. Same underlying
	// FlockServerState as quack_serve/stop above, but a different SQL
	// surface — flock_serve uses "flock:" defaults, flock_wait blocks
	// for daemon-mode init scripts (no quack_wait alias because
	// upstream never had one).
	loader.RegisterFunction(FlockServeFunction::GetFunction());
	loader.RegisterFunction(FlockStopFunction::GetFunction());
	loader.RegisterFunction(FlockWaitFunction::GetFunction());

	// the default authentication function
	ScalarFunction quack_check_token("quack_check_token",
	                                 {/* session id */ LogicalType::VARCHAR, /* auth string */ LogicalType::VARCHAR,
	                                  /* token */ LogicalType::VARCHAR},
	                                 LogicalType::BOOLEAN, QuackAuthToken);
	quack_check_token.SetVolatile();
	loader.RegisterFunction(quack_check_token);

	ScalarFunction rpc_authorization("quack_nop_authorization",
	                                 {/* session id */ LogicalType::VARCHAR, /* query string */ LogicalType::VARCHAR},
	                                 LogicalType::BOOLEAN, QuackDummyAuthorization);
	rpc_authorization.SetVolatile();
	loader.RegisterFunction(rpc_authorization);

	loader.RegisterFunction(QuackParseUriFunction::GetFunction());

	RegisterQuackSecretType(loader);

	loader.GetDatabaseInstance().GetLogManager().RegisterLogType(make_uniq<QuackLogType>());

	// Register the QuackStorageExtension under the "quack" key so
	// `ATTACH 'quack:host'` works for stock-quack clients. PR-2: the
	// storage_info no longer carries state (server lifecycle moved to
	// FlockServerState::Global()), but the registration shape is
	// unchanged for compatibility.
	auto ext = duckdb::make_shared_ptr<QuackStorageExtension>();
	ext->storage_info = duckdb::make_uniq<QuackStorageExtensionInfo>();
	StorageExtension::Register(loader.GetDatabaseInstance().config, QuackStorageExtensionInfo::STORAGE_EXTENSION_KEY,
	                           ext);

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
	config.AddExtensionOption("quack_authentication_function", "Name of a callback function for authentication",
	                          LogicalType::VARCHAR, Value("quack_check_token"), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("quack_authorization_function", "Name of a callback function for authorization",
	                          LogicalType::VARCHAR, Value("quack_nop_authorization"), nullptr, SetScope::GLOBAL);

	config.AddExtensionOption("quack_fetch_batch_chunks", "Maximum number of DataChunks returned per FETCH response",
	                          LogicalType::UBIGINT, Value::UBIGINT(12));

	// PR-4: cookie auth + CORS allow-list settings (per SPEC §9). The
	// cookie SIGNING KEY is deliberately NOT a SQL-readable setting in
	// v0.1 — exposing the HMAC secret to authorized SQL would let any
	// SQL caller mint cookies. AuthManager generates 32 random bytes
	// per process at first use; v0.2 reintroduces operator control
	// via the FLOCK_COOKIE_SIGNING_KEY environment variable.
	config.AddExtensionOption("flock_auth_cookie_ttl_s",
	                          "TTL in seconds for HMAC-signed flock_session cookies issued by /auth/login (default 12h)",
	                          LogicalType::UBIGINT, Value::UBIGINT(43200), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("flock_cors_origins",
	                          "Comma-separated allow-list of origins for cross-origin /quack, /auth/*, /sql, /info "
	                          "(empty = no cross-origin permitted; '*' is rejected)",
	                          LogicalType::VARCHAR, Value(""), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("flock_local_dev_mode",
	                          "When true AND bind is loopback: skip token requirement for same-Origin /ddb/* and UI catch-all requests "
	                          "(uses synthetic principal sha256(\"__FLOCK_LOCAL_DEV__\") for connection-pool keying); off by default",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::GLOBAL);

	// PR-3: UI extension settings. Keep upstream's `ui_*` names so
	// existing duckdb-ui tooling/docs still apply. The local-port
	// setting from upstream UI is intentionally NOT registered —
	// flock_serve takes the URI which encodes the port; ui_local_port
	// would be confusing dead state in flock.
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
	// flock-specific: the C++ Extension class identity must match the
	// loadable extension name so DuckDB's introspection / static-registry
	// bookkeeping doesn't see a mismatch. The class is still spelled
	// QuackExtension because PR-1 minimizes diff against upstream; only
	// the reported name changes.
	return "flock";
}

std::string QuackExtension::Version() const {
#ifdef EXT_VERSION_FLOCK
	return EXT_VERSION_FLOCK;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(flock, loader) {
	LoadInternal(loader);
}
}
