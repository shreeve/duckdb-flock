// PR-2 refactor of upstream duckdb-quack's quack_start_stop.cpp.
//
// quack_serve / quack_stop are kept as thin shims that delegate to
// FlockServerState::Global() (per AGENTS.md "Implementation roadmap":
// quack_* SQL functions stay as functional aliases of flock_*).
//
// quack_server_list adapts to single-server-per-process — at most one
// row in the result. SPEC §9 lists `flock_status()` as the eventual
// flock-side replacement; for PR-2 we keep quack_server_list working
// as the introspection surface so existing tooling doesn't break.

#include "duckdb/main/database.hpp"

#include "flock_auth.hpp"
#include "flock_http_server.hpp"
#include "flock_session.hpp" // SessionManager full definition for ActiveCount() in QuackServerList
#include "quack_startstop.hpp"

using namespace duckdb;

namespace {

struct QuackStartStopFunctionData : public TableFunctionData {
	QuackStartStopFunctionData() {
	}

	bool finished = false;
	QuackUri listen_uri;
	string token;
};

} // namespace

static unique_ptr<FunctionData> QuackServeBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<QuackStartStopFunctionData>();
	string listen_uri;
	if (input.inputs.empty()) {
		listen_uri = "quack:localhost";
	} else {
		auto &uri_value = input.inputs[0];
		if (uri_value.IsNull() || uri_value.GetValue<string>().empty()) {
			throw InvalidInputException("Invalid listen string specified");
		}
		listen_uri = uri_value.GetValue<string>();
	}

	auto allow_other_hostname = input.named_parameters.find("allow_other_hostname") != input.named_parameters.end() &&
	                            input.named_parameters["allow_other_hostname"].GetValue<bool>();

	bind_data->listen_uri = QuackUri(listen_uri, /* the server will always listen without SSL */ false);
	if (!allow_other_hostname && !bind_data->listen_uri.IsLocal()) {
		throw InvalidInputException(
		    "Only localhost is allowed as a Quack RPC hostname by default, set allow_other_hostname=true to override. "
		    "We strongly recommend reverse-proxying the Quack RPC when making it publicly available.");
	}

	return_types.emplace_back(LogicalType::VARCHAR);
	return_types.emplace_back(LogicalType::VARCHAR);
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("listen_uri");
	names.emplace_back("listen_url");
	names.emplace_back("auth_token");

	if (input.named_parameters.find("token") != input.named_parameters.end()) {
		bind_data->token = input.named_parameters["token"].GetValue<string>();
	} else {
		// PR-2: was QuackServer::GenerateRandomToken — now AuthManager (same impl).
		bind_data->token = AuthManager::GenerateRandomToken(*context.db);
	}
	// Validate at bind-time so length errors fail before the listener spawns.
	AuthManager::ValidateToken(bind_data->token);

	return std::move(bind_data);
}

static void QuackServe(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<QuackStartStopFunctionData>();
	if (bind_data.finished) {
		return;
	}

	// PR-2: was QuackStorageExtensionInfo::GetState(*context.db).CreateServer(...).
	// Now delegates to the process-global FlockServerState. Single-server-per-process
	// per SPEC §2 — a second quack_serve while one is running throws.
	FlockServerState::Global().Start(context, context.db, bind_data.listen_uri, bind_data.token);

	output.SetValue(0, 0, bind_data.listen_uri.Uri());
	output.SetValue(1, 0, bind_data.listen_uri.Http());
	output.SetValue(2, 0, bind_data.token);
	output.SetCardinality(1);
	bind_data.finished = true;
}

TableFunctionSet QuackServeFunction::GetFunction() {
	TableFunctionSet set("quack_serve");
	auto fun = TableFunction("quack_serve", {LogicalType::VARCHAR}, QuackServe, QuackServeBind);
	fun.named_parameters["disable_ssl"] = LogicalType::BOOLEAN;
	fun.named_parameters["allow_other_hostname"] = LogicalType::BOOLEAN;
	fun.named_parameters["token"] = LogicalType::VARCHAR;
	set.AddFunction(fun);
	fun.arguments.clear();
	set.AddFunction(fun);

	return set;
}

static unique_ptr<FunctionData> QuackStopBind(ClientContext & /* context */, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<QuackStartStopFunctionData>();
	auto &uri_value = input.inputs[0];
	if (uri_value.IsNull() || uri_value.GetValue<string>().empty()) {
		throw InvalidInputException("Invalid listen string specified");
	}
	bind_data->listen_uri =
	    QuackUri(uri_value.GetValue<string>(), /* not really, but we don't want to ask the user again */ true);
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");

	return std::move(bind_data);
}

static void QuackStop(ClientContext & /* context */, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<QuackStartStopFunctionData>();
	if (bind_data.finished) {
		return;
	}
	if (FlockServerState::Global().Stop(bind_data.listen_uri)) {
		output.data[0].SetValue(0, StringUtil::Format("Stopped listening on %s", bind_data.listen_uri.Uri()));
	} else {
		output.data[0].SetValue(0, StringUtil::Format("No server found listening on %s", bind_data.listen_uri.Uri()));
	}
	output.SetCardinality(1);
	bind_data.finished = true;
}

TableFunction QuackStopFunction::GetFunction() {
	return TableFunction("quack_stop", {LogicalType::VARCHAR}, QuackStop, QuackStopBind);
}

namespace {

struct QuackServerListFunctionData : public TableFunctionData {
	bool finished = false;
};

} // namespace

static unique_ptr<FunctionData> QuackServerListBind(ClientContext & /* context */, TableFunctionBindInput & /* input */,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("listen_uri");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("listen_url");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("host");
	return_types.emplace_back(LogicalType::USMALLINT);
	names.emplace_back("port");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("active_connections");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	names.emplace_back("info");
	return make_uniq<QuackServerListFunctionData>();
}

static void QuackServerList(ClientContext & /* context */, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<QuackServerListFunctionData>();
	if (bind_data.finished) {
		return;
	}

	// Single-server-per-process: at most one row.
	idx_t row = 0;
	FlockServerState::Global().WithCurrent([&](FlockHttpServer &srv) {
		auto &uri = srv.ListenUri();
		output.SetValue(0, row, Value(uri.Uri()));
		output.SetValue(1, row, Value(uri.Http()));
		output.SetValue(2, row, Value(uri.Host()));
		output.SetValue(3, row, Value::USMALLINT(uri.Port()));
		output.SetValue(4, row, Value::UBIGINT(srv.Sessions().ActiveCount()));

		vector<Value> keys;
		vector<Value> values;
		keys.emplace_back(Value("ipv6"));
		values.emplace_back(Value(uri.IPv6() ? "true" : "false"));
		output.SetValue(5, row,
		                Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(keys), std::move(values)));
		row++;
	});
	output.SetCardinality(row);
	bind_data.finished = true;
}

TableFunction QuackServerListFunction::GetFunction() {
	return TableFunction("quack_server_list", {}, QuackServerList, QuackServerListBind);
}
