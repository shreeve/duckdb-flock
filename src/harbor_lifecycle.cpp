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
	// True iff the operator passed `token := NULL` (unauthenticated
	// mode). Token is empty in that case; HarborHttpServer uses this
	// flag — not the empty token — to decide auth-bypass behavior.
	bool unauthenticated = false;
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

	// Custom-authn + token-arg incompatibility check.
	//
	// If a custom `harbor_authentication_function` is configured, the
	// static `token` argument is dead config: the callback decides
	// validity, not the static comparison. Silently honoring
	// `token := 'x'` in that case would mislead operators who expect
	// to use the returned token as a Bearer credential. Hard-error
	// here so the contradiction surfaces at config time.
	//
	// AuthManager snapshots the same setting at server-start, so what
	// we see here matches what the running server will use.
	bool has_custom_authn_fn = false;
	{
		auto db_locked = context.db ? context.db : nullptr;
		if (db_locked) {
			Value v;
			auto &cfg = DBConfig::GetConfig(*db_locked);
			auto pick = [&](const char *key) -> string {
				if (cfg.TryGetCurrentSetting(key, v) && !v.IsNull() &&
				    v.type().id() == LogicalTypeId::VARCHAR) {
					return v.GetValue<string>();
				}
				return string();
			};
			string fn = pick("harbor_authentication_function");
			if (fn.empty()) {
				fn = pick("quack_authentication_function");
			}
			has_custom_authn_fn = !fn.empty() && fn != "harbor_check_token" && fn != "quack_check_token";
		}
	}
	if (has_custom_authn_fn && input.named_parameters.find("token") != input.named_parameters.end()) {
		throw InvalidInputException(
		    "harbor_serve: token argument is not allowed when a custom "
		    "harbor_authentication_function is configured. The custom "
		    "callback decides credential validity; the static token would "
		    "be dead config.\n"
		    "  - Either omit the `token` argument:\n"
		    "        CALL harbor_serve('uri');\n"
		    "  - Or unset the custom authn function to use static-token auth:\n"
		    "        RESET GLOBAL harbor_authentication_function;\n"
		    "        CALL harbor_serve('uri', token := 'your-secret');");
	}

	// Token semantics on harbor_serve(uri, token := <value>):
	//
	//   omitted          → auto-generate a random token (default authn).
	//   token := 'x'     → use 'x' as the static token (default authn).
	//   token := NULL    → unauthenticated mode. All auth-bearing
	//                      routes accept any caller and assign the
	//                      synthetic 'harbor.local-dev' principal.
	//                      Refuses to start unless bound to loopback.
	//   token := ''      → hard error. Empty string is almost always
	//                      an env-var-plumbing accident; reject loudly.
	//
	// DuckDB's named-parameter binder implicitly casts numeric and
	// boolean values to strings (e.g. `token := 12345` becomes
	// `token := '12345'`) before this code sees them. The resulting
	// string is still a deliberate token of the operator's choosing.
	auto token_iter = input.named_parameters.find("token");
	if (token_iter == input.named_parameters.end()) {
		if (has_custom_authn_fn) {
			// Custom authn + token omitted → DON'T auto-generate. The
			// callback decides validity; an auto-generated token would
			// just be returned to the operator and ignored by the
			// callback. Result row's auth_token column will be NULL.
			bind_data->token = "";
			bind_data->unauthenticated = false;
		} else {
			// Default authn + token omitted → auto-generate.
			bind_data->token = AuthManager::GenerateRandomToken(*context.db);
			bind_data->unauthenticated = false;
			AuthManager::ValidateToken(bind_data->token);
		}
	} else if (token_iter->second.IsNull()) {
		// token := NULL → unauthenticated mode. Loopback bind required.
		if (!bind_data->listen_uri.IsLocal()) {
			throw InvalidInputException(
			    "harbor_serve: token := NULL (unauthenticated mode) requires a loopback bind, but '%s' is not "
			    "loopback. Either bind to localhost / 127.0.0.1 / [::1], or pass an explicit token.",
			    bind_data->listen_uri.Uri());
		}
		bind_data->token = "";
		bind_data->unauthenticated = true;
	} else {
		auto token_str = token_iter->second.GetValue<string>();
		if (token_str.empty()) {
			throw InvalidInputException(
			    "harbor_serve: token := '' is not allowed.\n"
			    "  - To require a static token, pass a non-empty string:\n"
			    "        token := 'your-secret'\n"
			    "  - To auto-generate a token, omit the argument:\n"
			    "        harbor_serve('uri')\n"
			    "  - To disable authentication (loopback only), pass NULL:\n"
			    "        token := NULL\n"
			    "Empty string is rejected because it is a common result of unset "
			    "environment variables or missing configuration, and would "
			    "otherwise silently disable authentication.");
		}
		bind_data->token = std::move(token_str);
		bind_data->unauthenticated = false;
		AuthManager::ValidateToken(bind_data->token);
	}

	return std::move(bind_data);
}

void HarborServe(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<HarborLifecycleBindData>();
	if (bind_data.finished) {
		return;
	}

	HarborServerState::Global().Start(context, context.db, bind_data.listen_uri, bind_data.token,
	                                  bind_data.unauthenticated);

	output.SetValue(0, 0, bind_data.listen_uri.Uri());
	output.SetValue(1, 0, bind_data.listen_uri.Http());
	// auth_token is NULL when there is no token the operator should
	// care about: unauthenticated mode (no auth) or custom-authn mode
	// (the callback decides, server token is dead config). A
	// placeholder like '' or '(none)' would be misleading.
	if (bind_data.token.empty()) {
		output.SetValue(2, 0, Value(LogicalType::VARCHAR));
	} else {
		output.SetValue(2, 0, bind_data.token);
	}
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

// No-arg variant. Resolves "the" running server's URI via
// HarborServerState::WithCurrent. Throws cleanly if no server is
// running. Single-server-per-process makes this unambiguous (per
// SPEC §2 / §9), and saves operators from having to remember the
// exact bind URI they passed to harbor_serve.
unique_ptr<FunctionData> HarborStopBindNoArg(ClientContext & /* context */,
                                              TableFunctionBindInput & /* input */,
                                              vector<LogicalType> &return_types,
                                              vector<string> &names) {
	auto bind_data = make_uniq<HarborLifecycleBindData>();
	bool found = false;
	HarborServerState::Global().WithCurrent([&](HarborHttpServer &server) {
		bind_data->listen_uri = server.ListenUri();
		found = true;
	});
	if (!found) {
		throw InvalidInputException(
		    "harbor_stop(): no server is currently running. "
		    "Use harbor_serve() first, or pass an explicit URI to harbor_stop(uri).");
	}
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

TableFunctionSet HarborStopFunction::GetFunction() {
	TableFunctionSet set("harbor_stop");
	// (a) harbor_stop() — no args; auto-discover the running server.
	set.AddFunction(TableFunction("harbor_stop", {}, HarborStop, HarborStopBindNoArg));
	// (b) harbor_stop(uri) — explicit URI; preserved for callers that
	//     scripted it with a literal URI, and for symmetry with quack_stop(uri).
	set.AddFunction(TableFunction("harbor_stop", {LogicalType::VARCHAR}, HarborStop, HarborStopBind));
	return set;
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
