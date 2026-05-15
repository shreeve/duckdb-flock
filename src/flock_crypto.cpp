#include "flock_crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cstring>
#include <stdexcept>

namespace duckdb {
namespace flock_crypto {

namespace {

constexpr char kHexChars[] = "0123456789abcdef";

// RFC 4648 §5 base64url alphabet (URL- and filename-safe; no padding
// emitted). Index: 6-bit value -> ASCII char.
constexpr char kB64UrlChars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Reverse map for base64url decode. 0xFF means "invalid byte". '=' is
// tolerated as an end-of-input padding marker and treated as terminator.
constexpr std::array<uint8_t, 256> BuildB64UrlReverseMap() {
	std::array<uint8_t, 256> map = {};
	for (auto &slot : map) {
		slot = 0xFF;
	}
	for (uint8_t i = 0; i < 64; ++i) {
		map[static_cast<uint8_t>(kB64UrlChars[i])] = i;
	}
	return map;
}

const std::array<uint8_t, 256> kB64UrlReverse = BuildB64UrlReverseMap();

std::string BytesToHex(const uint8_t *bytes, std::size_t n) {
	std::string out;
	out.resize(n * 2);
	for (std::size_t i = 0; i < n; ++i) {
		out[2 * i] = kHexChars[(bytes[i] >> 4) & 0x0F];
		out[2 * i + 1] = kHexChars[bytes[i] & 0x0F];
	}
	return out;
}

} // namespace

std::string Sha256Hex(const std::string &input) {
	std::array<uint8_t, SHA256_DIGEST_LENGTH> digest;
	// SHA256() is a stable convenience wrapper present in both
	// OpenSSL 1.1.x and 3.x. Avoids the EVP_MD_CTX dance for a
	// fixed-algorithm one-shot hash.
	SHA256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest.data());
	return BytesToHex(digest.data(), digest.size());
}

std::vector<uint8_t> HmacSha256(const std::vector<uint8_t> &key, const std::string &message) {
	std::vector<uint8_t> out(SHA256_DIGEST_LENGTH);
	unsigned int out_len = 0;
	// HMAC() is the legacy one-shot convenience; still supported in
	// OpenSSL 3.x (returns non-null on success). Avoids the
	// EVP_MAC_CTX_new + init + update + final sequence.
	auto *result = HMAC(EVP_sha256(),
	                    key.empty() ? "" : reinterpret_cast<const unsigned char *>(key.data()),
	                    static_cast<int>(key.size()),
	                    reinterpret_cast<const unsigned char *>(message.data()),
	                    message.size(),
	                    out.data(),
	                    &out_len);
	if (result == nullptr || out_len != SHA256_DIGEST_LENGTH) {
		throw std::runtime_error("HMAC-SHA256 failed");
	}
	return out;
}

std::string HmacSha256B64Url(const std::vector<uint8_t> &key, const std::string &message) {
	auto raw = HmacSha256(key, message);
	return Base64UrlEncode(raw);
}

std::vector<uint8_t> RandomBytes(std::size_t n) {
	std::vector<uint8_t> out(n);
	if (n == 0) {
		return out;
	}
	if (RAND_bytes(out.data(), static_cast<int>(n)) != 1) {
		// RAND_bytes failure is non-recoverable: the OpenSSL RNG has
		// either lost entropy or is misconfigured. Crashing the
		// crypto-using code path is correct; silently returning weak
		// randomness would create a security hole.
		throw std::runtime_error("RAND_bytes failed (no entropy?)");
	}
	return out;
}

std::string RandomHex(std::size_t n_bytes) {
	auto bytes = RandomBytes(n_bytes);
	return BytesToHex(bytes.data(), bytes.size());
}

std::string Base64UrlEncode(const std::vector<uint8_t> &bytes) {
	std::string out;
	if (bytes.empty()) {
		return out;
	}
	const std::size_t n = bytes.size();
	out.reserve(((n + 2) / 3) * 4); // overestimate; truncated below
	std::size_t i = 0;
	for (; i + 3 <= n; i += 3) {
		uint32_t v = (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8) |
		             static_cast<uint32_t>(bytes[i + 2]);
		out.push_back(kB64UrlChars[(v >> 18) & 0x3F]);
		out.push_back(kB64UrlChars[(v >> 12) & 0x3F]);
		out.push_back(kB64UrlChars[(v >> 6) & 0x3F]);
		out.push_back(kB64UrlChars[v & 0x3F]);
	}
	const std::size_t rem = n - i;
	if (rem == 1) {
		uint32_t v = static_cast<uint32_t>(bytes[i]) << 16;
		out.push_back(kB64UrlChars[(v >> 18) & 0x3F]);
		out.push_back(kB64UrlChars[(v >> 12) & 0x3F]);
		// no padding (RFC 4648 §5)
	} else if (rem == 2) {
		uint32_t v = (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
		out.push_back(kB64UrlChars[(v >> 18) & 0x3F]);
		out.push_back(kB64UrlChars[(v >> 12) & 0x3F]);
		out.push_back(kB64UrlChars[(v >> 6) & 0x3F]);
	}
	return out;
}

std::string Base64UrlEncode(const std::string &bytes) {
	std::vector<uint8_t> v(bytes.begin(), bytes.end());
	return Base64UrlEncode(v);
}

std::vector<uint8_t> Base64UrlDecode(const std::string &encoded) {
	// Strip trailing '=' padding if present (we don't emit it but
	// accept it on input for robustness against producers that do).
	std::size_t end = encoded.size();
	while (end > 0 && encoded[end - 1] == '=') {
		--end;
	}
	std::vector<uint8_t> out;
	out.reserve((end / 4) * 3 + 2);

	uint32_t buffer = 0;
	int bits = 0;
	for (std::size_t i = 0; i < end; ++i) {
		uint8_t c = static_cast<uint8_t>(encoded[i]);
		uint8_t v = kB64UrlReverse[c];
		if (v == 0xFF) {
			throw std::runtime_error("base64url: invalid character");
		}
		buffer = (buffer << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
		}
	}
	// Any leftover bits that weren't a full byte are discarded
	// (canonical base64 of length 4k+1 is invalid; lengths 4k+2 leave
	// 4 bits and 4k+3 leave 2 bits — both legitimate trailing-bit
	// scenarios). We don't validate that the trailing bits are zero;
	// HMAC verification catches any tampering.
	return out;
}

bool ConstantTimeEqual(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	if (a.empty()) {
		return true;
	}
	return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool ConstantTimeEqual(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	if (a.empty()) {
		return true;
	}
	return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string PrincipalIdHex(const std::string &client_token) {
	return Sha256Hex(client_token);
}

std::string PrincipalAbbrev(const std::string &principal_hex) {
	if (principal_hex.size() <= 8) {
		return principal_hex;
	}
	return principal_hex.substr(0, 8);
}

} // namespace flock_crypto
} // namespace duckdb
