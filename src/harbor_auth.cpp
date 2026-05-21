#include "harbor_auth.hpp"

#include "harbor_crypto.hpp"

#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"

// Match HarborHttpServer's openssl-enabled cpp-httplib so the
// httplib::Request type in AuthenticateRequest's signature is the
// same one route handlers receive. See header comment in
// src/include/harbor_http_server.hpp.
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace duckdb {

AuthManager::AuthManager(weak_ptr<DatabaseInstance> db_p, string server_token_p, bool unauthenticated_p)
    : db(std::move(db_p)), server_token(std::move(server_token_p)), unauthenticated(unauthenticated_p) {
}

AuthManager::~AuthManager() {
}

const string &AuthManager::LocalDevPrincipalId() {
	// Literal string (not a hash). Human-readable in audit logs;
	// stable across processes; no colon delimiter collision with
	// the __HARBOR_ADMIN__:resource:action format used elsewhere.
	static const string id = "harbor.local-dev";
	return id;
}

void AuthManager::ValidateToken(const string &token) {
	if (token.size() < 4) {
		throw InvalidInputException("harbor server token must be at least 4 characters long");
	}
}

namespace {

constexpr idx_t kTokenBytes = 16;            // 128 bits — server bearer token
constexpr idx_t kSigningKeyBytes = 32;       // 256 bits — HMAC-SHA256 key per RFC 4868
constexpr idx_t kCookieNonceBytes = 16;      // 128 bits — per-cookie randomness
constexpr const char *kCookieVersion = "v1"; // bump when format changes

string HexEncode(const data_t *bytes, idx_t n) {
	string result(n * 2, '\0');
	for (idx_t i = 0; i < n; i++) {
		result[2 * i] = Blob::HEX_TABLE[bytes[i] >> 4];
		result[2 * i + 1] = Blob::HEX_TABLE[bytes[i] & 0x0F];
	}
	return result;
}

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

// PR-6 follow-up (round 19): the centralized __HARBOR_ADMIN__
// default-deny in RunAuthorization decided "custom authz configured?"
// purely by setting presence (non-empty). That fails open when an
// operator explicitly sets the setting to one of the built-in NOP
// functions — `harbor_nop_authorization` or its quack-compat alias
// `quack_nop_authorization` — which always returns true. Quack users
// following the docs could reasonably set
// `quack_authorization_function='quack_nop_authorization'` and
// inadvertently expose admin endpoints.
//
// IsBuiltinNopAuthz() returns true iff the resolved fn name (after
// the same lower-casing/trimming) matches one of the known
// built-ins. Any operator who aliases the nop under a custom name
// is making an explicit policy choice and falls outside this rule
// (their custom policy is now the authoritative authz function).
bool IsBuiltinNopAuthz(const string &fn_name) {
	if (fn_name.empty()) {
		return false;
	}
	string normalized;
	normalized.reserve(fn_name.size());
	for (char c : fn_name) {
		if (std::isspace(static_cast<unsigned char>(c))) {
			continue;
		}
		normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	// Round-20 polish: strip a leading schema-qualifier prefix
	// (`main.`, `temp.`, etc.) so `main.harbor_nop_authorization`
	// is recognized as a built-in nop rather than slipping through
	// as "custom" and reopening the round-19 fail-open.
	auto dot = normalized.rfind('.');
	if (dot != string::npos) {
		normalized = normalized.substr(dot + 1);
	}
	return normalized == "harbor_nop_authorization" || normalized == "quack_nop_authorization";
}

// Trim ASCII whitespace from both ends. cpp-httplib's headers can
// arrive with leading spaces after ":"; settings strings can come
// from operators with stray whitespace.
string TrimAscii(const string &s) {
	size_t start = 0;
	while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
		++start;
	}
	size_t end = s.size();
	while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
		--end;
	}
	return s.substr(start, end - start);
}

// Extract the value of a specific cookie name from a Cookie header.
// Cookie header format: "name1=value1; name2=value2; ...". Returns
// empty string when the named cookie is not present.
string ExtractCookie(const string &cookie_header, const string &name) {
	if (cookie_header.empty() || name.empty()) {
		return string();
	}
	size_t pos = 0;
	while (pos < cookie_header.size()) {
		// Skip leading whitespace.
		while (pos < cookie_header.size() && std::isspace(static_cast<unsigned char>(cookie_header[pos]))) {
			++pos;
		}
		size_t eq = cookie_header.find('=', pos);
		size_t semi = cookie_header.find(';', pos);
		if (eq == string::npos || (semi != string::npos && eq > semi)) {
			break;
		}
		auto cur_name = cookie_header.substr(pos, eq - pos);
		size_t value_start = eq + 1;
		size_t value_end = (semi == string::npos) ? cookie_header.size() : semi;
		if (cur_name == name) {
			return cookie_header.substr(value_start, value_end - value_start);
		}
		if (semi == string::npos) {
			break;
		}
		pos = semi + 1;
	}
	return string();
}

// Validate principal_hex format (64 lowercase hex chars). We never
// trust decoded cookie segments without re-validating shape — even
// after HMAC verifies, malformed payloads should be rejected so
// downstream logic doesn't see surprises.
bool IsValidPrincipalHex(const string &s) {
	if (s.size() != 64) {
		return false;
	}
	for (char c : s) {
		bool is_digit = (c >= '0' && c <= '9');
		bool is_lower_hex = (c >= 'a' && c <= 'f');
		if (!is_digit && !is_lower_hex) {
			return false;
		}
	}
	return true;
}

uint64_t NowUnix() {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
	        .count());
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
	// v0.2 unauthenticated mode: skip the credential comparison entirely.
	// Quack still parses the CONNECTION_REQUEST frame correctly (caller-side);
	// only this comparison is skipped. Stock Quack clients sending a
	// non-empty AuthString continue to connect (the comparison just
	// short-circuits to true).
	if (unauthenticated) {
		return true;
	}
	auto db_locked = db.lock();
	if (!db_locked) {
		return false;
	}
	auto fn_name = GetSettingString(*db_locked, "harbor_authentication_function");
	if (fn_name.empty()) {
		fn_name = GetSettingString(*db_locked, "quack_authentication_function");
	}
	if (fn_name.empty()) {
		fn_name = "harbor_check_token";
	}
	auto sql = StringUtil::Format("SELECT %s(?, ?, ?)", fn_name);
	vector<Value> bind_values = {Value(session_id), Value(client_token), Value(server_token)};
	return EvaluateAuthQuery(*db_locked, sql, bind_values);
}

bool AuthManager::IsAdminAuthzCustomConfigured(DatabaseInstance &db) {
	auto harbor_fn = GetSettingString(db, "harbor_authorization_function");
	auto quack_fn = GetSettingString(db, "quack_authorization_function");
	const bool harbor_custom = !harbor_fn.empty() && !IsBuiltinNopAuthz(harbor_fn);
	const bool quack_custom = !quack_fn.empty() && !IsBuiltinNopAuthz(quack_fn);
	return harbor_custom || quack_custom;
}

bool AuthManager::RunAuthorization(const string &session_id, const string &query) {
	auto db_locked = db.lock();
	if (!db_locked) {
		return false;
	}

	// PR-6 — centralized default-deny on __HARBOR_ADMIN__:* synthetic
	// admin strings when no custom hook is configured. Per SPEC §7:
	// "Admin authorization is default-deny when no hook is configured."
	//
	// Detection rule:
	//   - Round 18 said: don't string-compare the RESOLVED fn name to
	//     "harbor_nop_authorization" — operator configs can
	//     schema-qualify, alias, or case-shift names in ways that
	//     would silently bypass.
	//   - Round 19 caught: pure setting-presence detection fails open
	//     when an operator EXPLICITLY sets harbor_authorization_function
	//     (or the quack-compat alias) to one of the BUILT-IN NOP
	//     functions. Quack users reasonably set
	//     `quack_authorization_function='quack_nop_authorization'`
	//     and inadvertently expose admin.
	//
	// Combined rule (encoded in IsAdminAuthzCustomConfigured): a
	// setting is "custom-configured" iff it is non-empty AND its
	// (lower-cased, whitespace-stripped, schema-prefix-stripped)
	// value is not a known built-in nop. Aliases under custom names
	// are treated as the operator's custom policy.
	//
	// Operators who genuinely want unrestricted admin access on a
	// trusted-network deployment flip harbor_allow_admin_without_authz=true.
	// harbor_serve emits a loud WARN log when the combination is in effect.
	//
	// Round-20 polish: only pay the two-setting-read cost when the
	// query is actually __HARBOR_ADMIN__-prefixed. Non-admin /sql and
	// /quack queries hit the fast resolve-fn path below directly.
	if (StringUtil::StartsWith(query, "__HARBOR_ADMIN__:") &&
	    !AuthManager::IsAdminAuthzCustomConfigured(*db_locked)) {
		bool allow_admin_without_authz = false;
		Value allow_setting;
		auto &config = DBConfig::GetConfig(*db_locked);
		if (config.TryGetCurrentSetting("harbor_allow_admin_without_authz", allow_setting) &&
		    !allow_setting.IsNull() && allow_setting.type().id() == LogicalTypeId::BOOLEAN) {
			allow_admin_without_authz = allow_setting.GetValue<bool>();
		}
		if (!allow_admin_without_authz) {
			return false;
		}
		// Falls through to harbor_nop_authorization, which always
		// returns true. This is the explicit operator opt-in path.
	}

	// Resolve the authz function name in Harbor-primary / Quack-compat order.
	auto fn_name = GetSettingString(*db_locked, "harbor_authorization_function");
	if (fn_name.empty()) {
		fn_name = GetSettingString(*db_locked, "quack_authorization_function");
	}
	if (fn_name.empty()) {
		fn_name = "harbor_nop_authorization";
	}
	auto sql = StringUtil::Format("SELECT %s(?, ?)", fn_name);
	vector<Value> bind_values = {Value(session_id), Value(query)};
	return EvaluateAuthQuery(*db_locked, sql, bind_values);
}

const std::vector<uint8_t> &AuthManager::CookieSigningKey() {
	std::lock_guard<std::mutex> lock(signing_key_mutex);
	if (signing_key.empty()) {
		// First use — generate ephemeral 32 random bytes. RAND_bytes
		// failure throws std::runtime_error, which propagates as a
		// 500 to whichever request triggered the first cookie issue/
		// verify. Operationally that's correct: with a broken RNG we
		// can't safely auth anyone.
		signing_key = harbor_crypto::RandomBytes(kSigningKeyBytes);
	}
	return signing_key;
}

string AuthManager::IssueCookie(const string &principal_hex, uint64_t ttl_s) {
	if (!IsValidPrincipalHex(principal_hex)) {
		// Should never happen in practice — callers always pass the
		// output of PrincipalIdHex(). Throw rather than silently
		// returning a malformed cookie.
		throw InvalidInputException("IssueCookie: principal_hex must be 64 lowercase hex chars");
	}
	auto expires_unix = NowUnix() + ttl_s;
	auto nonce = harbor_crypto::RandomBytes(kCookieNonceBytes);

	auto seg1 = harbor_crypto::Base64UrlEncode(principal_hex);                                  // payload identity
	auto seg2 = harbor_crypto::Base64UrlEncode(std::to_string(expires_unix));                   // ASCII unix seconds
	auto seg3 = harbor_crypto::Base64UrlEncode(nonce);                                          // anti-replay aid

	string mac_input = string(kCookieVersion) + "." + seg1 + "." + seg2 + "." + seg3;
	auto seg4 = harbor_crypto::HmacSha256B64Url(CookieSigningKey(), mac_input);

	return mac_input + "." + seg4;
}

AuthResult AuthManager::VerifyCookie(const string &cookie_value) {
	AuthResult result;
	if (cookie_value.empty()) {
		result.error_code = "MISSING_COOKIE";
		return result;
	}
	// Split on '.' into exactly 5 segments: version + 3 payload + mac.
	std::vector<string> segments;
	segments.reserve(5);
	size_t start = 0;
	for (size_t i = 0; i < cookie_value.size(); ++i) {
		if (cookie_value[i] == '.') {
			segments.push_back(cookie_value.substr(start, i - start));
			start = i + 1;
		}
	}
	segments.push_back(cookie_value.substr(start));
	if (segments.size() != 5) {
		result.error_code = "BAD_COOKIE_FORMAT";
		return result;
	}
	if (segments[0] != kCookieVersion) {
		result.error_code = "BAD_COOKIE_VERSION";
		return result;
	}

	// Recompute HMAC over the on-the-wire prefix bytes (NOT a
	// re-encoded reconstruction). This is critical: any byte-for-byte
	// difference between issuance and verification (e.g. different
	// base64url canonicalization) would silently break verification.
	string mac_input =
	    segments[0] + "." + segments[1] + "." + segments[2] + "." + segments[3];
	std::vector<uint8_t> expected_mac;
	std::vector<uint8_t> actual_mac;
	try {
		actual_mac = harbor_crypto::Base64UrlDecode(segments[4]);
		expected_mac = harbor_crypto::HmacSha256(CookieSigningKey(), mac_input);
	} catch (...) {
		result.error_code = "BAD_COOKIE_FORMAT";
		return result;
	}
	if (!harbor_crypto::ConstantTimeEqual(expected_mac, actual_mac)) {
		result.error_code = "BAD_COOKIE_SIG";
		return result;
	}

	// Decode payload segments. Reject malformed shapes even after
	// successful HMAC verification: an attacker who somehow got the
	// signing key would still hit these checks.
	string principal_hex;
	uint64_t expires_unix = 0;
	try {
		auto p_bytes = harbor_crypto::Base64UrlDecode(segments[1]);
		principal_hex.assign(reinterpret_cast<const char *>(p_bytes.data()), p_bytes.size());
		auto e_bytes = harbor_crypto::Base64UrlDecode(segments[2]);
		string e_str(reinterpret_cast<const char *>(e_bytes.data()), e_bytes.size());
		expires_unix = std::stoull(e_str);
	} catch (...) {
		result.error_code = "BAD_COOKIE_PAYLOAD";
		return result;
	}
	if (!IsValidPrincipalHex(principal_hex)) {
		result.error_code = "BAD_COOKIE_PAYLOAD";
		return result;
	}
	// Expire-at-or-after the recorded timestamp (inclusive expiry,
	// matching RFC 6265-ish convention). A cookie issued with
	// expires_unix == now is already expired the moment it's verified.
	if (NowUnix() >= expires_unix) {
		result.error_code = "COOKIE_EXPIRED";
		return result;
	}

	result.ok = true;
	result.principal_id = principal_hex;
	result.source = AuthSource::kCookie;
	return result;
}

AuthResult AuthManager::AuthenticateRequest(const duckdb_httplib_openssl::Request &req,
                                            const string &synthetic_session_id) {
	// v0.2 unauthenticated mode (harbor_serve(uri, token := NULL) on
	// loopback): short-circuit before looking at any credentials. ALL
	// presented credentials (Bearer / Cookie / X-Harbor-Token) are
	// ignored — stale cookies must never produce a different principal
	// than fresh requests, or audit-trail meaning is lost.
	if (unauthenticated) {
		AuthResult ar;
		ar.ok = true;
		ar.principal_id = LocalDevPrincipalId();
		ar.source = AuthSource::kLocalDev;
		return ar;
	}

	auto try_explicit_token = [&](const string &header_name, AuthSource source,
	                              const string &raw_value) -> AuthResult {
		AuthResult ar;
		ar.source = source;
		string token = raw_value;
		if (source == AuthSource::kBearer) {
			// "Bearer <token>" — case-sensitive scheme per RFC 6750.
			// PR-7c (round-23 review): non-Bearer schemes return
			// UNSUPPORTED_AUTH_SCHEME (distinct from MISSING_CREDENTIAL,
			// which means "no Authorization header AND no X-Harbor-Token
			// AND no cookie"). The handler maps both to 401 but the
			// errorCode tells operator tooling whether they sent the
			// wrong scheme (e.g. accidentally Basic on a misconfigured
			// reverse proxy) vs forgot the credential entirely.
			constexpr const char *kBearerPrefix = "Bearer ";
			constexpr size_t kBearerPrefixLen = 7;
			if (token.size() < kBearerPrefixLen ||
			    token.compare(0, kBearerPrefixLen, kBearerPrefix) != 0) {
				ar.error_code = "UNSUPPORTED_AUTH_SCHEME";
				return ar;
			}
			token = TrimAscii(token.substr(kBearerPrefixLen));
		} else {
			token = TrimAscii(token);
		}
		if (token.empty()) {
			ar.error_code = "MISSING_CREDENTIAL";
			return ar;
		}
		if (!RunAuthentication(synthetic_session_id, token)) {
			ar.error_code = "INVALID_TOKEN";
			return ar;
		}
		ar.ok = true;
		ar.principal_id = harbor_crypto::PrincipalIdHex(token);
		return ar;
	};

	// 1. Authorization: Bearer <token> — explicit and highest priority.
	//    Do NOT fall back to cookie on failure (round-11 review): a
	//    caller that supplied an explicit credential and got it wrong
	//    must see a 401, not silently authenticate as a different
	//    principal via ambient browser state.
	auto auth_header = req.get_header_value("Authorization");
	if (!auth_header.empty()) {
		return try_explicit_token("Authorization", AuthSource::kBearer, auth_header);
	}

	// 2. X-Harbor-Token — explicit alternative for environments where
	//    Authorization is awkward (e.g. some proxies strip it).
	auto x_harbor = req.get_header_value("X-Harbor-Token");
	if (!x_harbor.empty()) {
		return try_explicit_token("X-Harbor-Token", AuthSource::kXHarborToken, x_harbor);
	}

	// 3. Cookie: harbor_session=<value> — implicit; for the in-browser
	//    UI flow.
	auto cookie_header = req.get_header_value("Cookie");
	if (!cookie_header.empty()) {
		auto cookie_value = ExtractCookie(cookie_header, "harbor_session");
		if (!cookie_value.empty()) {
			return VerifyCookie(cookie_value);
		}
	}

	AuthResult none;
	none.error_code = "MISSING_CREDENTIAL";
	return none;
}

void AuthManager::ValidateCorsOrigin(const string &origin) {
	if (origin.empty()) {
		throw InvalidInputException("harbor_cors_origins entry is empty");
	}
	// Must be scheme://host[:port], no path, no query, no fragment.
	// We don't fully validate URL syntax — we just enforce the shape
	// the browser sends in Origin headers (which is exactly that).
	auto scheme_end = origin.find("://");
	if (scheme_end == string::npos || scheme_end == 0) {
		throw InvalidInputException("harbor_cors_origins entry '%s' has no scheme://host", origin);
	}
	auto rest = origin.substr(scheme_end + 3);
	if (rest.empty() ||
	    rest.find('/') != string::npos ||
	    rest.find('?') != string::npos ||
	    rest.find('#') != string::npos) {
		throw InvalidInputException(
		    "harbor_cors_origins entry '%s' must be scheme://host[:port] with no path/query/fragment",
		    origin);
	}
	auto scheme = origin.substr(0, scheme_end);
	if (scheme != "http" && scheme != "https") {
		throw InvalidInputException("harbor_cors_origins entry '%s' must use http or https scheme", origin);
	}
}

void AuthManager::InitCorsConfig(const string &cors_origins_setting) {
	cors_allowed_origins.clear();
	auto trimmed_setting = TrimAscii(cors_origins_setting);
	if (trimmed_setting.empty()) {
		return;
	}
	if (trimmed_setting == "*") {
		// SPEC §7: "the server refuses to start if harbor_cors_origins='*'".
		// Wildcard origin combined with credentials is forbidden by
		// CORS spec and by us — silently honoring it would be a
		// browser-side privilege escalation.
		throw InvalidInputException("harbor_cors_origins='*' is forbidden (wildcard with credentials is "
		                            "unsafe). List specific origins instead.");
	}
	std::vector<string> parts = StringUtil::Split(trimmed_setting, ",");
	for (auto &raw : parts) {
		auto entry = TrimAscii(raw);
		if (entry.empty()) {
			continue;
		}
		ValidateCorsOrigin(entry);
		cors_allowed_origins.push_back(entry);
	}
}

CorsDecision AuthManager::ResolveCorsOrigin(const string &request_origin) const {
	CorsDecision decision;
	if (request_origin.empty() || cors_allowed_origins.empty()) {
		return decision;
	}
	for (const auto &allowed : cors_allowed_origins) {
		if (request_origin == allowed) {
			decision.allowed = true;
			decision.origin = allowed;
			return decision;
		}
	}
	return decision;
}

} // namespace duckdb
