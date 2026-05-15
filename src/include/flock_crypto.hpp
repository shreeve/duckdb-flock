#pragma once

// flock_crypto — thin C++ wrapper around OpenSSL libcrypto for the
// fixed set of primitives flock needs:
//
//   - SHA-256 hashing (principal_id derivation, SPEC §6)
//   - HMAC-SHA256 (flock_session cookie signing, SPEC §7)
//   - CSPRNG bytes (cookie signing key, cookie nonces, future session IDs)
//   - base64url encoding/decoding (cookie segment encoding)
//   - constant-time byte comparison (HMAC verification)
//
// Design rules:
//   - All primitives are stateless free functions in
//     duckdb::flock_crypto. No process-static state lives here; the
//     ephemeral cookie signing key lives on AuthManager.
//   - RandomBytes / RandomHex throw std::runtime_error if RAND_bytes
//     fails. RAND_bytes failure is not recoverable from inside this
//     library — the OpenSSL RNG has lost entropy or is misconfigured,
//     and no caller can sanely continue.
//   - Inputs/outputs use std::string (binary-safe; std::string can hold
//     arbitrary bytes including NUL) and std::vector<uint8_t> (preferred
//     for raw binary payloads where byte-vs-character distinction
//     matters). HMAC inputs are std::string because HmacSha256's
//     payload is the on-the-wire ASCII-prefix bytes.
//   - base64url uses the URL-safe alphabet (RFC 4648 §5: '-' and '_'
//     instead of '+' and '/') with NO padding. Cookie segments are
//     dot-separated; '=' padding would just be visual noise.
//   - ConstantTimeEqual ALWAYS compares full length even if one side
//     is shorter, returning false. Length itself is leaked (it must
//     be — there is no way to compare without knowing both lengths).
//     Callers that need length-secrecy use a fixed-size buffer.
//
// Threading: every function is reentrant. OpenSSL's EVP_*, HMAC, and
// RAND_bytes APIs are thread-safe in OpenSSL 1.1.x+ (which is what
// vcpkg ships and what flock requires).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace flock_crypto {

// SHA-256 hex digest (lowercase, 64 chars) of `input`.
std::string Sha256Hex(const std::string &input);

// HMAC-SHA256 raw output (32 bytes) using `key` and `message`.
std::vector<uint8_t> HmacSha256(const std::vector<uint8_t> &key, const std::string &message);

// HMAC-SHA256 output base64url-encoded (no padding, ~43 chars).
std::string HmacSha256B64Url(const std::vector<uint8_t> &key, const std::string &message);

// `n` cryptographically random bytes via OpenSSL RAND_bytes.
// Throws std::runtime_error on RAND_bytes failure.
std::vector<uint8_t> RandomBytes(std::size_t n);

// 2*n_bytes lowercase hex chars from RandomBytes(n_bytes).
std::string RandomHex(std::size_t n_bytes);

// base64url encode (RFC 4648 §5, no padding).
std::string Base64UrlEncode(const std::vector<uint8_t> &bytes);
std::string Base64UrlEncode(const std::string &bytes);

// base64url decode. Accepts input with or without trailing '=' padding
// (we don't emit it but tolerate it on input). Throws std::runtime_error
// on malformed input.
std::vector<uint8_t> Base64UrlDecode(const std::string &encoded);

// Constant-time byte equality. Returns false immediately if lengths
// differ (lengths are not secret; the byte content is). Otherwise
// compares all bytes via CRYPTO_memcmp.
bool ConstantTimeEqual(const std::string &a, const std::string &b);
bool ConstantTimeEqual(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b);

// principal_id = Sha256Hex(client_token). Per SPEC §6, this is the
// canonical derivation rule and matches between bearer-token and
// cookie auth — so a bearer caller and a cookie caller presenting
// the same underlying token map to the same principal.
std::string PrincipalIdHex(const std::string &client_token);

// First 8 hex chars of `principal_hex`, for log lines. Never log the
// full principal_hex — it's a deterministic function of the secret
// token, and (with enough log-history correlation) might let an
// attacker confirm-or-deny a token guess.
std::string PrincipalAbbrev(const std::string &principal_hex);

} // namespace flock_crypto
} // namespace duckdb
