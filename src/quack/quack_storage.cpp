// PR-2 refactor of upstream duckdb-quack's quack_storage.cpp.
//
// All server-management code (CreateServer / StopServer / ListServers
// + the multi-server map) is gone — that's FlockServerState::Global()
// now (single-server-per-process per SPEC §2). What's left here is
// the ATTACH 'quack:host' codepath, which is unchanged semantically.

#include "duckdb/main/database.hpp"

#include "quack_storage.hpp"
#include "storage/quack_catalog.hpp"
#include "storage/quack_transaction_manager.hpp"
#include "quack_uri.hpp"

namespace duckdb {

namespace {

unique_ptr<Catalog> QuackAttach(optional_ptr<StorageExtensionInfo> /* storage_info */, ClientContext &context,
                                AttachedDatabase &db, const string &name, AttachInfo &info,
                                AttachOptions &attach_options) {
	// info.path may or may not already carry the "quack:" prefix.
	auto uri = StringUtil::StartsWith(info.path, "quack:") ? info.path : "quack:" + info.path;
	auto initial_uri = QuackUri(uri);

	// no ssl on local by default
	auto enable_ssl = !initial_uri.IsLocal();
	if (attach_options.options.find("disable_ssl") != attach_options.options.end()) {
		enable_ssl = !attach_options.options["disable_ssl"].GetValue<bool>();
	}
	string token;
	if (attach_options.options.find("token") != attach_options.options.end()) {
		token = attach_options.options["token"].GetValue<string>();
	}
	return make_uniq<QuackCatalog>(db, QuackUri(uri, enable_ssl), context, token);
}

unique_ptr<TransactionManager> QuackCreateTransactionManager(optional_ptr<StorageExtensionInfo> /* storage_info */,
                                                              AttachedDatabase &db, Catalog &catalog) {
	auto &quack_catalog = catalog.Cast<QuackCatalog>();
	return make_uniq<QuackTransactionManager>(db, quack_catalog);
}

} // namespace

QuackStorageExtension::QuackStorageExtension() {
	attach = QuackAttach;
	create_transaction_manager = QuackCreateTransactionManager;
}

} // namespace duckdb
