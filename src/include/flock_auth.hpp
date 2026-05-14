#pragma once

// AuthManager — server-token + auth/authz callback resolution.
//
// Lifted from upstream QuackServer's auth-related members and the
// static EvaluateAuthQuery free function. PR-2 keeps the existing
// semantics (BOOLEAN-returning auth callback per CONNECTION_REQUEST,
// BOOLEAN-returning authz callback per PREPARE/APPEND) — the SPEC §7
// principal-id / cookie / admin-authz model lands in PR-3 once the
// auth-cookie flow is in place. The cryptographic primitives that PR-3
// needs (SHA-256 / HMAC-SHA256 / CSPRNG) come from OpenSSL libcrypto
// via src/flock_crypto.{cpp,hpp} (introduced in PR-3, not here).

#include "duckdb/common/common.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class DatabaseInstance;

class AuthManager {
public:
	AuthManager(weak_ptr<DatabaseInstance> db, string server_token);
	~AuthManager();

	AuthManager(const AuthManager &) = delete;
	AuthManager &operator=(const AuthManager &) = delete;

	const string &ServerToken() const {
		return server_token;
	}

	// Throws InvalidInputException if token is shorter than 4 chars.
	// Same contract as upstream QuackServer::ValidateToken.
	static void ValidateToken(const string &token);

	// CSPRNG-backed 16-byte (128-bit) token, hex-encoded (32 chars).
	// Uses db.GetEncryptionUtil() — not OpenSSL — because that's what
	// upstream uses and it's already wired up. PR-3's flock_crypto.cpp
	// will likely route this through OpenSSL's RAND_bytes for unification.
	static string GenerateRandomToken(DatabaseInstance &db);

	// Run the authentication callback against a transient Connection.
	// Reads the function name from the `quack_authentication_function`
	// setting (PR-3 will add `flock_authentication_function` as an
	// alias setting pointing at the same default) and invokes it as
	// `SELECT <fn_name>(?, ?, ?)` with `(session_id, client_token,
	// server_token)` bound as prepared parameters — never string-formatted.
	// Returns false on any failure path (NULL, non-bool, exception, false).
	bool RunAuthentication(const string &session_id, const string &client_token);

	// Run the authorization callback against a transient Connection.
	// Reads the function name from `quack_authorization_function` and
	// invokes it as `SELECT <fn_name>(?, ?)` with `(session_id, query)`
	// bound as prepared parameters. `query` is the user's SQL for
	// /quack PREPARE, the synthetic INSERT for APPEND, or in PR-3+ the
	// synthetic `__FLOCK_ADMIN__:resource:action` strings for admin
	// endpoints. Returns false on any failure path.
	bool RunAuthorization(const string &session_id, const string &query);

private:
	weak_ptr<DatabaseInstance> db;
	string server_token;
};

} // namespace duckdb
