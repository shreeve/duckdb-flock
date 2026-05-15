#include "harbor_lifecycle.hpp"

#include "harbor_auth.hpp"
#include "harbor_http_server.hpp"
#include "quack_uri.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

// -- Shared bind data ----------------------------------------------------

namespace {

struct HarborLifecycleBindData : public TableFunctionData {
	bool finished = false;
	QuackUri listen_uri;
	string token;
};

struct HarborWaitBindData : public TableFunctionData {
	bool finished = false;
};

} // namespace

// -- harbor_serve ---------------------------------------------------------

namespace {

unique_ptr<FunctionData> HarborServeBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<HarborLifecycleBindData>();
	string listen_uri;
	if (input.inputs.empty()) {
		listen_uri = "harbor:localhost";
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
		    "Only localhost is allowed as a harbor RPC hostname by default, set allow_other_hostname=true to override. "
		    "We strongly recommend reverse-proxying harbor when making it publicly available.");
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

void HarborServe(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<HarborLifecycleBindData>();
	if (bind_data.finished) {
		return;
	}

	HarborServerState::Global().Start(context, context.db, bind_data.listen_uri, bind_data.token);

	output.SetValue(0, 0, bind_data.listen_uri.Uri());
	output.SetValue(1, 0, bind_data.listen_uri.Http());
	output.SetValue(2, 0, bind_data.token);
	output.SetCardinality(1);
	bind_data.finished = true;
}

} // namespace

TableFunctionSet HarborServeFunction::GetFunction() {
	TableFunctionSet set("harbor_serve");
	auto fun = TableFunction("harbor_serve", {LogicalType::VARCHAR}, HarborServe, HarborServeBind);
	fun.named_parameters["disable_ssl"] = LogicalType::BOOLEAN; // accepted-but-ignored for upstream parity
	fun.named_parameters["allow_other_hostname"] = LogicalType::BOOLEAN;
	fun.named_parameters["token"] = LogicalType::VARCHAR;
	set.AddFunction(fun);
	fun.arguments.clear();
	set.AddFunction(fun);
	return set;
}

// -- harbor_stop ----------------------------------------------------------

namespace {

unique_ptr<FunctionData> HarborStopBind(ClientContext & /* context */, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<HarborLifecycleBindData>();
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

void HarborStop(ClientContext & /* context */, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<HarborLifecycleBindData>();
	if (bind_data.finished) {
		return;
	}
	if (HarborServerState::Global().Stop(bind_data.listen_uri)) {
		output.data[0].SetValue(0, StringUtil::Format("Stopped listening on %s", bind_data.listen_uri.Uri()));
	} else {
		output.data[0].SetValue(0, StringUtil::Format("No server found listening on %s", bind_data.listen_uri.Uri()));
	}
	output.SetCardinality(1);
	bind_data.finished = true;
}

} // namespace

TableFunction HarborStopFunction::GetFunction() {
	return TableFunction("harbor_stop", {LogicalType::VARCHAR}, HarborStop, HarborStopBind);
}

// -- harbor_wait ----------------------------------------------------------

namespace {

unique_ptr<FunctionData> HarborWaitBind(ClientContext & /* context */, TableFunctionBindInput & /* input */,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("ok");
	return make_uniq<HarborWaitBindData>();
}

void HarborWait(ClientContext & /* context */, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<HarborWaitBindData>();
	if (bind_data.finished) {
		return;
	}
	// Blocks until HarborServerState::Stop() (called from harbor_stop /
	// quack_stop on another session) or process signal (SIGTERM/SIGINT).
	// Throws InvalidInputException if no server is currently running.
	auto ok = HarborServerState::Global().Wait();
	output.SetValue(0, 0, Value::BOOLEAN(ok));
	output.SetCardinality(1);
	bind_data.finished = true;
}

} // namespace

TableFunction HarborWaitFunction::GetFunction() {
	return TableFunction("harbor_wait", {}, HarborWait, HarborWaitBind);
}

} // namespace duckdb
