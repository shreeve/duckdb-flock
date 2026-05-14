#include "flock_lifecycle.hpp"

#include "flock_auth.hpp"
#include "flock_http_server.hpp"
#include "quack_uri.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

// -- Shared bind data ----------------------------------------------------

namespace {

struct FlockLifecycleBindData : public TableFunctionData {
	bool finished = false;
	QuackUri listen_uri;
	string token;
};

struct FlockWaitBindData : public TableFunctionData {
	bool finished = false;
};

} // namespace

// -- flock_serve ---------------------------------------------------------

namespace {

unique_ptr<FunctionData> FlockServeBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<FlockLifecycleBindData>();
	string listen_uri;
	if (input.inputs.empty()) {
		listen_uri = "flock:localhost";
	} else {
		auto &uri_value = input.inputs[0];
		if (uri_value.IsNull() || uri_value.GetValue<string>().empty()) {
			throw InvalidInputException("Invalid listen string specified");
		}
		listen_uri = uri_value.GetValue<string>();
	}

	auto allow_other_hostname = input.named_parameters.find("allow_other_hostname") != input.named_parameters.end() &&
	                            input.named_parameters["allow_other_hostname"].GetValue<bool>();

	bind_data->listen_uri = QuackUri(listen_uri, /* server always listens without SSL */ false);
	if (!allow_other_hostname && !bind_data->listen_uri.IsLocal()) {
		throw InvalidInputException(
		    "Only localhost is allowed as a flock RPC hostname by default, set allow_other_hostname=true to override. "
		    "We strongly recommend reverse-proxying flock when making it publicly available.");
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
		bind_data->token = AuthManager::GenerateRandomToken(*context.db);
	}
	// Validate at bind-time: a length error here fails before the listener
	// thread is spawned, so the SQL caller sees the error clearly.
	AuthManager::ValidateToken(bind_data->token);

	return std::move(bind_data);
}

void FlockServe(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<FlockLifecycleBindData>();
	if (bind_data.finished) {
		return;
	}

	FlockServerState::Global().Start(context, context.db, bind_data.listen_uri, bind_data.token);

	output.SetValue(0, 0, bind_data.listen_uri.Uri());
	output.SetValue(1, 0, bind_data.listen_uri.Http());
	output.SetValue(2, 0, bind_data.token);
	output.SetCardinality(1);
	bind_data.finished = true;
}

} // namespace

TableFunctionSet FlockServeFunction::GetFunction() {
	TableFunctionSet set("flock_serve");
	auto fun = TableFunction("flock_serve", {LogicalType::VARCHAR}, FlockServe, FlockServeBind);
	fun.named_parameters["disable_ssl"] = LogicalType::BOOLEAN; // accepted-but-ignored for upstream parity
	fun.named_parameters["allow_other_hostname"] = LogicalType::BOOLEAN;
	fun.named_parameters["token"] = LogicalType::VARCHAR;
	set.AddFunction(fun);
	fun.arguments.clear();
	set.AddFunction(fun);
	return set;
}

// -- flock_stop ----------------------------------------------------------

namespace {

unique_ptr<FunctionData> FlockStopBind(ClientContext & /* context */, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<FlockLifecycleBindData>();
	auto &uri_value = input.inputs[0];
	if (uri_value.IsNull() || uri_value.GetValue<string>().empty()) {
		throw InvalidInputException("Invalid listen string specified");
	}
	// Second arg of QuackUri is `ssl`; for the stop-side we don't really
	// care since we look up by canonical-uri, but match upstream's
	// pattern of passing `true` here.
	bind_data->listen_uri = QuackUri(uri_value.GetValue<string>(), true);
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return std::move(bind_data);
}

void FlockStop(ClientContext & /* context */, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<FlockLifecycleBindData>();
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

} // namespace

TableFunction FlockStopFunction::GetFunction() {
	return TableFunction("flock_stop", {LogicalType::VARCHAR}, FlockStop, FlockStopBind);
}

// -- flock_wait ----------------------------------------------------------

namespace {

unique_ptr<FunctionData> FlockWaitBind(ClientContext & /* context */, TableFunctionBindInput & /* input */,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("ok");
	return make_uniq<FlockWaitBindData>();
}

void FlockWait(ClientContext & /* context */, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<FlockWaitBindData>();
	if (bind_data.finished) {
		return;
	}
	// Blocks until FlockServerState::Stop() (called from flock_stop /
	// quack_stop on another session) or process signal (SIGTERM/SIGINT).
	// Throws InvalidInputException if no server is currently running.
	auto ok = FlockServerState::Global().Wait();
	output.SetValue(0, 0, Value::BOOLEAN(ok));
	output.SetCardinality(1);
	bind_data.finished = true;
}

} // namespace

TableFunction FlockWaitFunction::GetFunction() {
	return TableFunction("flock_wait", {}, FlockWait, FlockWaitBind);
}

} // namespace duckdb
