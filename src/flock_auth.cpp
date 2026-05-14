#include "flock_auth.hpp"

#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"

namespace duckdb {

AuthManager::AuthManager(weak_ptr<DatabaseInstance> db_p, string server_token_p)
    : db(std::move(db_p)), server_token(std::move(server_token_p)) {
}

AuthManager::~AuthManager() {
}

void AuthManager::ValidateToken(const string &token) {
	if (token.size() < 4) {
		throw InvalidInputException("flock server token must be at least 4 characters long");
	}
}

namespace {

constexpr idx_t kTokenBytes = 16; // 128 bits

string HexEncode(const data_t *bytes, idx_t n) {
	string result(n * 2, '\0');
	for (idx_t i = 0; i < n; i++) {
		result[2 * i] = Blob::HEX_TABLE[bytes[i] >> 4];
		result[2 * i + 1] = Blob::HEX_TABLE[bytes[i] & 0x0F];
	}
	return result;
}

// Read a VARCHAR setting from the DBConfig. The auth/authz function
// names are registered as VARCHAR DBConfig options at extension load
// time, so the lookup is expected to succeed; an empty string means
// no callback is configured (which the caller treats as deny).
string GetSettingString(DatabaseInstance &db, const string &setting_name) {
	Value setting_val;
	auto &config = DBConfig::GetConfig(db);

	auto lookup_result = config.TryGetCurrentSetting(setting_name, setting_val);
	if (!lookup_result || setting_val.IsNull() || setting_val.type().id() != LogicalTypeId::VARCHAR) {
		return string();
	}
	auto setting_str = setting_val.GetValue<string>();
	return setting_str;
}

// Run the auth/authz query against a transient Connection.
//
// The SQL string contains `?` placeholders; `values` are bound as
// prepared statement parameters via PreparedStatement::Execute(vector<Value>&).
// Critical: do NOT format `values` into the SQL string — the auth
// callback name comes from a DBConfig setting (formatted via %s), but
// the user-controlled session id, client token, and query text are
// always parameter-bound to defeat injection.
//
// Returns true iff the query succeeds AND the (0, 0) result is a
// non-NULL boolean true. Any other path (lookup miss, query error,
// NULL/non-bool result, exception) returns false.
//
// Note: takes `values` by value (not const-ref) because
// PreparedStatement::Execute requires a mutable lvalue reference. The
// caller passes a vector that's local to the per-request call anyway,
// so the move-or-copy is cheap.
bool EvaluateAuthQuery(DatabaseInstance &db, const string &sql, vector<Value> values) {
	try {
		Connection dummy_connection(db);
		auto prepared = dummy_connection.Prepare(sql);
		if (!prepared || prepared->HasError()) {
			return false;
		}
		auto auth_result = prepared->Execute(values);
		if (!auth_result || auth_result->HasError()) {
			return false;
		}
		auto auth_chunk = auth_result->Fetch();
		if (!auth_chunk || auth_chunk->size() == 0) {
			return false;
		}
		auto cell = auth_chunk->GetValue(0, 0);
		if (cell.IsNull() || cell.type().id() != LogicalTypeId::BOOLEAN) {
			return false;
		}
		return cell.GetValue<bool>();
	} catch (...) {
		return false;
	}
}

} // namespace

string AuthManager::GenerateRandomToken(DatabaseInstance &db) {
	auto encryption_util = db.GetEncryptionUtil(false);
	auto metadata = make_uniq<EncryptionStateMetadata>(EncryptionTypes::GCM, kTokenBytes,
	                                                   EncryptionTypes::EncryptionVersion::NONE);
	auto rng = encryption_util->CreateEncryptionState(std::move(metadata));

	data_t bytes[kTokenBytes];
	rng->GenerateRandomData(bytes, kTokenBytes);
	return HexEncode(bytes, kTokenBytes);
}

bool AuthManager::RunAuthentication(const string &session_id, const string &client_token) {
	auto db_locked = db.lock();
	if (!db_locked) {
		return false;
	}
	auto fn_name = GetSettingString(*db_locked, "quack_authentication_function");
	if (fn_name.empty()) {
		return false;
	}
	// Function-name from setting is interpolated (it's a SQL identifier,
	// not user-controlled data). Session id, client token, server token are
	// bound as prepared parameters.
	auto sql = StringUtil::Format("SELECT %s(?, ?, ?)", fn_name);
	vector<Value> bind_values = {Value(session_id), Value(client_token), Value(server_token)};
	return EvaluateAuthQuery(*db_locked, sql, bind_values);
}

bool AuthManager::RunAuthorization(const string &session_id, const string &query) {
	auto db_locked = db.lock();
	if (!db_locked) {
		return false;
	}
	auto fn_name = GetSettingString(*db_locked, "quack_authorization_function");
	if (fn_name.empty()) {
		return false;
	}
	auto sql = StringUtil::Format("SELECT %s(?, ?)", fn_name);
	vector<Value> bind_values = {Value(session_id), Value(query)};
	return EvaluateAuthQuery(*db_locked, sql, bind_values);
}

} // namespace duckdb
