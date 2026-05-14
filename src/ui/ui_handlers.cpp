// PR-3: UiHandlers — refactored from upstream duckdb-ui's HttpServer.
//
// What's preserved verbatim from upstream:
//   - HandleGetLocalEvents body (SSE via EventDispatcher)
//   - HandleGetLocalToken body (Referer + loopback gate)
//   - HandleInterrupt body
//   - HandleRun + DoHandleRun + HandleTokenize bodies (binary protocol)
//   - HandleProxyGet body (was HandleGet upstream — forwards to ui.duckdb.org)
//   - All helpers (ReadContent, SetResponseContent, SetResponseEmptyResult,
//     SetResponseErrorResult, InitClientFromParams)
//
// What's new / refactored:
//   - No singleton, no atexit, no Run() / Start() / Stop() lifecycle
//   - No embedded duckdb_httplib_openssl::Server (lives on FlockHttpServer)
//   - Constructor builds the allowed-origins set (per GPT-5.5 round 9
//     catch #4 — single-string Origin check breaks for non-loopback bind)
//   - SSE handler uses shared_ptr<ActiveRequestGuard> captured by the
//     chunked content provider closure (per GPT-5.5 round 9 catch on
//     ActiveRequestGuard lifetime — stack-local guard would die before
//     the provider runs)
//   - Shutdown() method called by FlockHttpServer::Close() before the
//     active-request drain (per GPT-5.5 round 9 catch #1)

#include "ui_handlers.hpp"

#include "event_dispatcher.hpp"
#include "settings.hpp"
#include "state.hpp"
#include "utils/encoding.hpp"
#include "utils/env.hpp"
#include "utils/helpers.hpp"
#include "utils/md_helpers.hpp"
#include "utils/serialization.hpp"
#include "version.hpp"
#include "watcher.hpp"

#include "flock_http_server.hpp"

#include <duckdb/common/http_util.hpp>
#include <duckdb/common/serializer/binary_serializer.hpp>
#include <duckdb/common/serializer/memory_stream.hpp>
#include <duckdb/main/attached_database.hpp>
#include <duckdb/main/client_data.hpp>
#include <duckdb/parser/parsed_data/create_table_info.hpp>
#include <duckdb/parser/parser.hpp>
#if DUCKDB_VERSION_AT_LEAST(1, 5, 0)
#include <duckdb/common/enums/database_modification_type.hpp>
#include <duckdb/main/settings.hpp>
#endif

#include <climits>
#include <sstream>
#include <thread>

namespace httplib = duckdb_httplib_openssl;

namespace duckdb {
namespace ui {

const char *UiHandlers::UiExtensionVersion() {
	return UI_EXTENSION_VERSION;
}

UiHandlers::UiHandlers(FlockHttpServer &server_p, weak_ptr<DatabaseInstance> db_p, ClientContext &context)
    : server(server_p), ddb_instance(std::move(db_p)) {
	remote_url = GetRemoteUrl(context);
	allowed_origins = ComputeAllowedOrigins();
	local_url_prefix = ComputeLocalUrlPrefix();
	user_agent = StringUtil::Format("flock-ui/%s-%s(%s)", DuckDB::LibraryVersion(), UI_EXTENSION_VERSION,
	                                DuckDB::Platform());
	polling_interval_ms = GetPollingInterval(context);

	auto &http_util = HTTPUtil::Get(*context.db);
	// FIXME upstream: https://github.com/duckdb/duckdb/pull/17655 will remove `unused`
	http_params = http_util.InitializeParameters(context, "unused");

	event_dispatcher = make_uniq<EventDispatcher>();
	watcher = make_uniq<Watcher>(ddb_instance, *event_dispatcher, polling_interval_ms);
}

UiHandlers::~UiHandlers() {
	// Defensive — Shutdown() should have been called by
	// FlockHttpServer::Close() already, but cover the path where
	// UiHandlers is destroyed without Close() being called (e.g.,
	// construction failure mid-RegisterBuiltinHandlers).
	Shutdown();
}

void UiHandlers::Shutdown() {
	if (shutdown_called) {
		return;
	}
	shutdown_called = true;
	// Order matters: stop the producer (Watcher) first so it doesn't
	// emit into a dispatcher that's about to close, then close the
	// dispatcher (which wakes any /localEvents WaitEvent calls so the
	// SSE handler lambda returns and releases its ActiveRequestGuard).
	if (watcher) {
		watcher->Stop();
	}
	if (event_dispatcher) {
		event_dispatcher->Close();
	}
}

std::vector<std::string> UiHandlers::ComputeAllowedOrigins() const {
	std::vector<std::string> origins;
	auto port = server.ListenUri().Port();
	// Always allow the loopback variants — these are what browsers
	// typically use for local dev.
	origins.push_back(StringUtil::Format("http://localhost:%d", port));
	origins.push_back(StringUtil::Format("http://127.0.0.1:%d", port));
	origins.push_back(StringUtil::Format("http://[::1]:%d", port));

	// If the bind host is concrete (not 0.0.0.0 or empty), include it
	// too. With 0.0.0.0 binding we can't validate against an arbitrary
	// remote Host header — that's PR-4's CORS allow-list territory.
	auto bind_host = server.ListenUri().Host();
	if (!bind_host.empty() && bind_host != "0.0.0.0" && bind_host != "localhost" && bind_host != "127.0.0.1" &&
	    bind_host != "::1") {
		// IPv6 hosts need brackets in URL-form
		auto host_form =
		    server.ListenUri().IPv6() ? StringUtil::Format("[%s]", bind_host) : bind_host;
		origins.push_back(StringUtil::Format("http://%s:%d", host_form, port));
	}
	return origins;
}

std::string UiHandlers::ComputeLocalUrlPrefix() const {
	// Per upstream UI: /localToken Referer check matches the START of
	// the local URL (because Referer includes the path). Use the
	// localhost variant — that's what browsers send for local dev.
	return StringUtil::Format("http://localhost:%d", server.ListenUri().Port());
}

bool UiHandlers::IsAllowedOrigin(const std::string &origin) const {
	if (origin.empty()) {
		return false;
	}
	for (const auto &allowed : allowed_origins) {
		if (origin == allowed) {
			return true;
		}
	}
	return false;
}

bool UiHandlers::IsBoundLocally() const {
	auto host = server.ListenUri().Host();
	return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

shared_ptr<DatabaseInstance> UiHandlers::LockDatabaseInstance() {
	return ddb_instance.lock();
}

// Adapted from
// https://github.com/duckdb/duckdb/blob/1f8b6839ea7864c3e3fb020574f67384cb58124c/src/main/http/http_util.cpp#L129-L147
// (not currently exposed as a public DuckDB API).
void UiHandlers::InitClientFromParams(httplib::Client &client) {
	auto sec = static_cast<time_t>(http_params->timeout);
	auto usec = static_cast<time_t>(http_params->timeout_usec);
	client.set_keep_alive(true);
	client.set_write_timeout(sec, usec);
	client.set_read_timeout(sec, usec);
	client.set_connection_timeout(sec, usec);

	if (!http_params->http_proxy.empty()) {
		client.set_proxy(http_params->http_proxy, static_cast<int>(http_params->http_proxy_port));
		if (!http_params->http_proxy_username.empty()) {
			client.set_proxy_basic_auth(http_params->http_proxy_username, http_params->http_proxy_password);
		}
	}
}

std::string UiHandlers::ReadContent(const httplib::ContentReader &content_reader) {
	std::ostringstream oss;
	content_reader([&](const char *data, size_t data_length) {
		oss.write(data, data_length);
		return true;
	});
	return oss.str();
}

void UiHandlers::SetResponseContent(httplib::Response &res, const MemoryStream &content) {
	auto data = content.GetData();
	auto length = content.GetPosition();
	res.set_content(reinterpret_cast<const char *>(data), length, "application/octet-stream");
}

void UiHandlers::SetResponseEmptyResult(httplib::Response &res) {
	EmptyResult empty_result;
	MemoryStream response_content;
	BinarySerializer::Serialize(empty_result, response_content);
	SetResponseContent(res, response_content);
}

void UiHandlers::SetResponseErrorResult(httplib::Response &res, const std::string &error) {
	ErrorResult error_result;
	error_result.error = error;
	MemoryStream response_content;
	BinarySerializer::Serialize(error_result, response_content);
	SetResponseContent(res, response_content);
}

// ---------------- Route handlers ----------------

void UiHandlers::HandleGetLocalEvents(const httplib::Request &, httplib::Response &res) {
	// SSE: chunked content provider holds a shared_ptr<ActiveRequestGuard>
	// that lives for the duration of the streaming, not just the
	// route lambda. Set up at the call site (Register) — see comment
	// there. This method is called from inside the route lambda which
	// has already configured the chunked provider; nothing more to
	// do here at the synchronous handler entry point.
	(void)res;
}

void UiHandlers::HandleGetLocalToken(const httplib::Request &req, httplib::Response &res) {
	// /localToken is conditionally available only when bound locally
	// (per SPEC §7). This protects MotherDuck token disclosure on
	// remote-bound deployments.
	if (!IsBoundLocally()) {
		res.status = 404;
		return;
	}

	// GET requests don't include Origin, so use Referer instead.
	// Referer includes the path, so only compare the start.
	auto referer = req.get_header_value("Referer");
	if (referer.compare(0, local_url_prefix.size(), local_url_prefix) != 0) {
		res.status = 401;
		return;
	}

	auto db = LockDatabaseInstance();
	if (!db) {
		res.status = 500;
		res.set_content("Database was invalidated, UI needs to be restarted", "text/plain");
		return;
	}

	Connection connection {*db};
	try {
		auto token = GetMDToken(connection);
		res.status = 200;
		res.set_content(token, "text/plain");
	} catch (std::exception &ex) {
		res.status = 500;
		res.set_content("Could not get token: " + std::string(ex.what()), "text/plain");
	}
}

void UiHandlers::HandleProxyGet(const httplib::Request &req, httplib::Response &res) {
	// Outbound HTTPS client to remote_url (default ui.duckdb.org).
	// TODO: Can this be created once and shared?
	httplib::Client client(remote_url);
	InitClientFromParams(client);

	if (IsEnvEnabled("ui_disable_server_certificate_verification")) {
		client.enable_server_certificate_verification(false);
	}

	httplib::Headers headers = {{"User-Agent", user_agent}};
	auto cookie = req.get_header_value("Cookie");
	if (!cookie.empty()) {
		headers.emplace("Cookie", cookie);
	}

	auto result = client.Get(req.path, req.params, headers);
	if (!result) {
		res.status = 500;
		res.set_content("Could not fetch: '" + req.path + "' from '" + remote_url +
		                    "': " + to_string(result.error()),
		                "text/plain");
		return;
	}

	res = result.value();

	// If this is the config request, return additional information.
	if (req.path == "/config") {
		res.set_header("X-DuckDB-Version", DuckDB::LibraryVersion());
		res.set_header("X-DuckDB-Platform", DuckDB::Platform());
		// The UI looks for this to select the appropriate DuckDB mode (HTTP or
		// Wasm).
		res.set_header("X-DuckDB-UI-Extension-Version", UI_EXTENSION_VERSION);
	}

	// httplib will set Content-Length, remove it so it is not duplicated.
	res.headers.erase("Content-Length");
}

void UiHandlers::HandleInterrupt(const httplib::Request &req, httplib::Response &res) {
	if (!IsAllowedOrigin(req.get_header_value("Origin"))) {
		res.status = 401;
		return;
	}

	auto connection_name = req.get_header_value("X-DuckDB-UI-Connection-Name");

	auto db = LockDatabaseInstance();
	if (!db) {
		res.status = 404;
		return;
	}

	auto connection = UIStorageExtensionInfo::GetState(*db).FindConnection(connection_name);
	if (!connection) {
		res.status = 404;
		return;
	}

	connection->Interrupt();

	SetResponseEmptyResult(res);
}

void UiHandlers::HandleRun(const httplib::Request &req, httplib::Response &res,
                            const httplib::ContentReader &content_reader) {
	try {
		DoHandleRun(req, res, content_reader);
	} catch (const std::exception &ex) {
		SetResponseErrorResult(res, ex.what());
	}
}

void UiHandlers::DoHandleRun(const httplib::Request &req, httplib::Response &res,
                              const httplib::ContentReader &content_reader) {
	if (!IsAllowedOrigin(req.get_header_value("Origin"))) {
		res.status = 401;
		return;
	}

	auto connection_name = req.get_header_value("X-DuckDB-UI-Connection-Name");
	auto database_name_option = DecodeBase64(req.get_header_value("X-DuckDB-UI-Database-Name"));
	auto schema_name_option = DecodeBase64(req.get_header_value("X-DuckDB-UI-Schema-Name"));

	std::vector<std::string> parameter_values;
	auto parameter_count_string = req.get_header_value("X-DuckDB-UI-Parameter-Count");
	if (!parameter_count_string.empty()) {
		auto parameter_count = std::stoi(parameter_count_string);
		for (auto i = 0; i < parameter_count; ++i) {
			auto parameter_value =
			    DecodeBase64(req.get_header_value(StringUtil::Format("X-DuckDB-UI-Parameter-Value-%d", i)));
			parameter_values.push_back(parameter_value);
		}
	}

	auto result_row_limit = INT_MAX;
	auto result_row_limit_string = req.get_header_value("X-DuckDB-UI-Result-Row-Limit");
	if (!result_row_limit_string.empty()) {
		result_row_limit = std::stoi(result_row_limit_string);
	}

	auto result_database_name_option = DecodeBase64(req.get_header_value("X-DuckDB-UI-Result-Database-Name"));
	auto result_schema_name_option = DecodeBase64(req.get_header_value("X-DuckDB-UI-Result-Schema-Name"));
	auto result_table_name = DecodeBase64(req.get_header_value("X-DuckDB-UI-Result-Table-Name"));

	auto result_table_row_limit = result_table_name.empty() ? 0 : INT_MAX;
	auto result_table_row_limit_string = req.get_header_value("X-DuckDB-UI-Result-Table-Row-Limit");
	if (!result_table_name.empty() && !result_table_row_limit_string.empty()) {
		result_table_row_limit = std::stoi(result_table_row_limit_string);
	}

	auto errors_as_json_string = req.get_header_value("X-DuckDB-UI-Errors-As-JSON");

	std::string content = ReadContent(content_reader);

	auto db = LockDatabaseInstance();
	if (!db) {
		SetResponseErrorResult(res, "Database was invalidated, UI needs to be restarted");
		return;
	}

	auto connection = UIStorageExtensionInfo::GetState(*db).FindOrCreateConnection(*db, connection_name);
	auto &context = *connection->context;
	if (!errors_as_json_string.empty()) {
#if DUCKDB_VERSION_AT_LEAST(1, 5, 0)
		auto &config = DBConfig::GetConfig(context);
		config.user_settings.SetUserSetting(ErrorsAsJSONSetting::SettingIndex, true);
#else
		auto &config = ClientConfig::GetConfig(context);
		config.errors_as_json = errors_as_json_string == "true";
#endif
	}

	if (!database_name_option.empty() || !schema_name_option.empty()) {
		auto schema_name = schema_name_option.empty() ? DEFAULT_SCHEMA : schema_name_option;
		context.RunFunctionInTransaction([&] {
			duckdb::ClientData::Get(context).catalog_search_path->Set(
			    {database_name_option, schema_name}, duckdb::CatalogSetPathType::SET_SCHEMA);
		});
	}

	vector<unique_ptr<SQLStatement>> statements;
	try {
		statements = connection->ExtractStatements(content);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		SetResponseErrorResult(res, error.RawMessage());
		return;
	}

	auto statement_count = statements.size();

	if (statement_count == 0) {
		SetResponseErrorResult(res, "No statements");
		return;
	}

	if (statement_count > 1) {
		for (size_t i = 0; i < statement_count - 1; ++i) {
			auto pending = connection->PendingQuery(std::move(statements[i]), true);
			if (pending->HasError()) {
				SetResponseErrorResult(res, pending->GetError());
				return;
			}
			auto exec_result = PendingExecutionResult::RESULT_NOT_READY;
			while (!PendingQueryResult::IsResultReady(exec_result)) {
				exec_result = pending->ExecuteTask();
				if (exec_result == PendingExecutionResult::BLOCKED ||
				    exec_result == PendingExecutionResult::NO_TASKS_AVAILABLE) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
			switch (exec_result) {
			case PendingExecutionResult::EXECUTION_ERROR:
				SetResponseErrorResult(res, pending->GetError());
				return;
			case PendingExecutionResult::EXECUTION_FINISHED:
			case PendingExecutionResult::RESULT_READY:
				pending->Execute();
				break;
			default:
				SetResponseErrorResult(
				    res, StringUtil::Format("Unexpected PendingExecutionResult: %s", exec_result));
				return;
			}
		}
	}

	auto &statement_to_run = statements[statement_count - 1];

	unique_ptr<PendingQueryResult> pending;
	if (parameter_values.size() > 0) {
		auto prepared = connection->Prepare(std::move(statement_to_run));
		if (prepared->HasError()) {
			SetResponseErrorResult(res, prepared->GetError());
			return;
		}
		vector<Value> values;
		for (auto &parameter_value : parameter_values) {
			// TODO: support non-string parameters?
			values.push_back(Value(parameter_value));
		}
		pending = prepared->PendingQuery(values, true);
	} else {
		pending = connection->PendingQuery(std::move(statement_to_run), true);
	}

	if (pending->HasError()) {
		SetResponseErrorResult(res, pending->GetError());
		return;
	}

	auto exec_result = PendingExecutionResult::RESULT_NOT_READY;
	while (!PendingQueryResult::IsResultReady(exec_result)) {
		exec_result = pending->ExecuteTask();
		if (exec_result == PendingExecutionResult::BLOCKED ||
		    exec_result == PendingExecutionResult::NO_TASKS_AVAILABLE) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	switch (exec_result) {
	case PendingExecutionResult::EXECUTION_ERROR:
		SetResponseErrorResult(res, pending->GetError());
		break;
	case PendingExecutionResult::EXECUTION_FINISHED:
	case PendingExecutionResult::RESULT_READY: {
		auto result = pending->Execute();

		unique_ptr<duckdb::Connection> appender_connection;
		unique_ptr<duckdb::Appender> appender;

		if (!result_table_name.empty()) {
			auto result_database_name = result_database_name_option.empty() ? "memory" : result_database_name_option;
			auto result_schema_name = result_schema_name_option.empty() ? "main" : result_schema_name_option;

			auto result_table_info =
			    make_uniq<duckdb::CreateTableInfo>(result_database_name, result_schema_name, result_table_name);
			for (idx_t i = 0; i < result->names.size(); i++) {
				result_table_info->columns.AddColumn(ColumnDefinition(result->names[i], result->types[i]));
			}

			appender_connection = make_uniq<duckdb::Connection>(*db);
			auto appender_context = appender_connection->context;
			appender_context->RunFunctionInTransaction([&] {
				auto &catalog = duckdb::Catalog::GetCatalog(*appender_context, result_database_name);
#if DUCKDB_MAJOR_VERSION == 1 && DUCKDB_MINOR_VERSION < 5
				MetaTransaction::Get(*appender_context).ModifyDatabase(catalog.GetAttached());
#else
				MetaTransaction::Get(*appender_context)
				    .ModifyDatabase(catalog.GetAttached(), DatabaseModificationType::CREATE_CATALOG_ENTRY);
#endif
				catalog.CreateTable(*appender_context, std::move(result_table_info));
			});

			appender = make_uniq<duckdb::Appender>(*appender_connection, result_database_name, result_schema_name,
			                                       result_table_name);
		}

		SuccessResult success_result;
		success_result.column_names_and_types = {std::move(result->names), std::move(result->types)};

		auto row_limit = std::max(result_row_limit, result_table_row_limit);
		auto rows_fetched = 0;
		auto rows_appended = 0;
		auto rows_in_result = 0;
		unique_ptr<duckdb::DataChunk> chunk;
		while (rows_fetched < row_limit) {
			chunk = result->Fetch();
			if (!chunk) {
				break;
			}
			rows_fetched += chunk->size();
			if (appender && rows_appended < result_table_row_limit) {
				duckdb::DataChunk *chunk_to_append = chunk.get();
				duckdb::DataChunk chunk_prefix;
				const idx_t rows_left = result_table_row_limit - rows_appended;
				if (chunk->size() > rows_left) {
					chunk_prefix.InitializeEmpty(chunk->GetTypes());
					chunk_prefix.Reference(*chunk);
					chunk_prefix.Slice(0, rows_left);
					chunk_to_append = &chunk_prefix;
				}
				appender->AppendDataChunk(*chunk_to_append);
				rows_appended += chunk_to_append->size();
			}
			if (rows_in_result < result_row_limit) {
				duckdb::DataChunk *chunk_to_add = chunk.get();
				duckdb::DataChunk chunk_prefix;
				const idx_t rows_left = result_row_limit - rows_in_result;
				if (chunk->size() > rows_left) {
					chunk_prefix.InitializeEmpty(chunk->GetTypes());
					chunk_prefix.Reference(*chunk);
					chunk_prefix.Slice(0, rows_left);
					chunk_to_add = &chunk_prefix;
				}
				success_result.chunks.push_back(
				    {static_cast<uint16_t>(chunk_to_add->size()), std::move(chunk_to_add->data)});
				rows_in_result += chunk_to_add->size();
			}
		}

		if (appender) {
			appender->Close();
		}

		MemoryStream success_response_content;
		BinarySerializer::Serialize(success_result, success_response_content);
		SetResponseContent(res, success_response_content);
		break;
	}
	default:
		SetResponseErrorResult(res, StringUtil::Format("Unexpected PendingExecutionResult: %s", exec_result));
		break;
	}
}

void UiHandlers::HandleTokenize(const httplib::Request &req, httplib::Response &res,
                                 const httplib::ContentReader &content_reader) {
	if (!IsAllowedOrigin(req.get_header_value("Origin"))) {
		res.status = 401;
		return;
	}

	std::string content = ReadContent(content_reader);

	auto tokens = Parser::Tokenize(content);

	TokenizeResult result;
	result.offsets.reserve(tokens.size());
	result.types.reserve(tokens.size());
	for (auto token : tokens) {
		result.offsets.push_back(token.start);
		result.types.push_back(token.type);
	}

	MemoryStream response_content;
	BinarySerializer::Serialize(result, response_content);
	SetResponseContent(res, response_content);
}

// ---------------- Route registration ----------------

void UiHandlers::Register(duckdb_httplib_openssl::Server &http) {
	auto *self = this;

	// Start the catalog watcher (pushes "catalog changed" events to
	// /localEvents SSE clients).
	if (watcher) {
		watcher->Start();
	}

	// /localEvents — Server-Sent Events stream. The chunked content
	// provider runs LATER (after this lambda returns), so the
	// ActiveRequestGuard must outlive the lambda. Use a shared_ptr
	// captured by the provider closure.
	http.Get("/localEvents", [self](const httplib::Request &, httplib::Response &res) {
		auto guard = std::make_shared<FlockHttpServer::ActiveRequestGuard>(self->server);
		res.set_chunked_content_provider("text/event-stream",
		                                  [self, guard](size_t /* offset */, httplib::DataSink &sink) -> bool {
			                                  if (self->event_dispatcher && self->event_dispatcher->WaitEvent(&sink)) {
				                                  return true;
			                                  }
			                                  sink.done();
			                                  return false;
		                                  });
	});

	// /localToken — Referer + loopback gated.
	http.Get("/localToken", [self](const httplib::Request &req, httplib::Response &res) {
		FlockHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleGetLocalToken(req, res);
	});

	// /ddb/interrupt — Origin-checked.
	http.Post("/ddb/interrupt", [self](const httplib::Request &req, httplib::Response &res) {
		FlockHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleInterrupt(req, res);
	});

	// /ddb/run — Origin-checked. Binary protocol via BinarySerializer.
	http.Post("/ddb/run", [self](const httplib::Request &req, httplib::Response &res,
	                              const httplib::ContentReader &content_reader) {
		FlockHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleRun(req, res, content_reader);
	});

	// /ddb/tokenize — Origin-checked. Binary protocol.
	http.Post("/ddb/tokenize", [self](const httplib::Request &req, httplib::Response &res,
	                                   const httplib::ContentReader &content_reader) {
		FlockHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleTokenize(req, res, content_reader);
	});

	// GET /.* catch-all proxy. MUST be the LAST route registered (cpp-httplib
	// resolves in registration order — catch-all would shadow earlier
	// routes). Caller (FlockHttpServer::RegisterBuiltinHandlers)
	// invokes UiHandlers::Register AFTER QuackHandlers and AdminHandlers.
	http.Get("/.*", [self](const httplib::Request &req, httplib::Response &res) {
		FlockHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleProxyGet(req, res);
	});
}

} // namespace ui
} // namespace duckdb
