#include "sql_json_writer.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace duckdb {
namespace flock_sql {

namespace {

constexpr char kHex[] = "0123456789abcdef";

// Validate a UTF-8 sequence starting at s[i], returning the number of
// bytes consumed (1..4) on success or 0 on invalid sequence. The
// caller advances `i` by the return value, or by 1 on a 0 return
// (with U+FFFD substitution).
std::size_t ValidateUtf8(const char *s, std::size_t i, std::size_t len) {
	auto byte = static_cast<unsigned char>(s[i]);
	if (byte < 0x80) {
		return 1;
	}
	if ((byte & 0xE0) == 0xC0) {
		// 2-byte sequence: 110xxxxx 10xxxxxx
		if (i + 1 >= len) {
			return 0;
		}
		auto b1 = static_cast<unsigned char>(s[i + 1]);
		if ((b1 & 0xC0) != 0x80) {
			return 0;
		}
		// Reject overlong: byte must be ≥ 0xC2 (otherwise codepoint < 0x80)
		if (byte < 0xC2) {
			return 0;
		}
		return 2;
	}
	if ((byte & 0xF0) == 0xE0) {
		// 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx
		if (i + 2 >= len) {
			return 0;
		}
		auto b1 = static_cast<unsigned char>(s[i + 1]);
		auto b2 = static_cast<unsigned char>(s[i + 2]);
		if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
			return 0;
		}
		// Reject overlong / surrogate codepoints
		if (byte == 0xE0 && b1 < 0xA0) {
			return 0; // overlong
		}
		if (byte == 0xED && b1 >= 0xA0) {
			return 0; // surrogate U+D800..U+DFFF
		}
		return 3;
	}
	if ((byte & 0xF8) == 0xF0) {
		// 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
		if (i + 3 >= len) {
			return 0;
		}
		auto b1 = static_cast<unsigned char>(s[i + 1]);
		auto b2 = static_cast<unsigned char>(s[i + 2]);
		auto b3 = static_cast<unsigned char>(s[i + 3]);
		if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
			return 0;
		}
		// Reject overlong / out-of-range (>U+10FFFF)
		if (byte == 0xF0 && b1 < 0x90) {
			return 0; // overlong
		}
		if (byte == 0xF4 && b1 >= 0x90) {
			return 0; // > U+10FFFF
		}
		if (byte > 0xF4) {
			return 0; // > U+10FFFF
		}
		return 4;
	}
	return 0;
}

// Emit \uFFFD (UTF-8: EF BF BD).
void EmitReplacement(std::string &buf) {
	buf.append("\xEF\xBF\xBD", 3);
}

} // namespace

void JsonWriter::Reset() {
	buf.clear();
	frames.clear();
	first_in_frame = true;
}

std::string JsonWriter::Take() {
	std::string out;
	out.swap(buf);
	frames.clear();
	first_in_frame = true;
	return out;
}

void JsonWriter::BeforeValue() {
	if (frames.empty()) {
		// ROOT: nothing to do.
		return;
	}
	auto top = static_cast<Frame>(frames.back());
	if (top == Frame::ARRAY) {
		if (!first_in_frame) {
			buf.push_back(',');
		}
		first_in_frame = false;
	} else if (top == Frame::OBJECT_VALUE) {
		// We just emitted "key": and now this value finishes the pair.
		// Transition back to OBJECT_KEY (next thing must be a key or
		// EndObject).
		frames.back() = static_cast<char>(Frame::OBJECT_KEY);
		first_in_frame = false;
	}
	// OBJECT_KEY at value-emit time would be a misuse; we just emit
	// silently and let the resulting JSON be invalid (caller bug).
}

void JsonWriter::BeforeKey() {
	if (frames.empty() || static_cast<Frame>(frames.back()) != Frame::OBJECT_KEY) {
		return; // misuse; caller bug
	}
	if (!first_in_frame) {
		buf.push_back(',');
	}
	first_in_frame = false;
}

void JsonWriter::BeginObject() {
	BeforeValue();
	buf.push_back('{');
	frames.push_back(static_cast<char>(Frame::OBJECT_KEY));
	first_in_frame = true;
}

void JsonWriter::EndObject() {
	if (frames.empty()) {
		return;
	}
	buf.push_back('}');
	frames.pop_back();
	// After ending a container, the parent's first_in_frame is no
	// longer "first" — we just emitted a value.
	first_in_frame = false;
}

void JsonWriter::BeginArray() {
	BeforeValue();
	buf.push_back('[');
	frames.push_back(static_cast<char>(Frame::ARRAY));
	first_in_frame = true;
}

void JsonWriter::EndArray() {
	if (frames.empty()) {
		return;
	}
	buf.push_back(']');
	frames.pop_back();
	first_in_frame = false;
}

void JsonWriter::Key(const char *key) {
	BeforeKey();
	WriteEscapedString(key, std::strlen(key));
	buf.push_back(':');
	frames.back() = static_cast<char>(Frame::OBJECT_VALUE);
}

void JsonWriter::Key(const std::string &key) {
	BeforeKey();
	WriteEscapedString(key.data(), key.size());
	buf.push_back(':');
	frames.back() = static_cast<char>(Frame::OBJECT_VALUE);
}

void JsonWriter::Null() {
	BeforeValue();
	buf.append("null", 4);
}

void JsonWriter::Bool(bool v) {
	BeforeValue();
	buf.append(v ? "true" : "false");
}

void JsonWriter::Int64(int64_t v) {
	BeforeValue();
	char tmp[32];
	auto n = std::snprintf(tmp, sizeof(tmp), "%" PRId64, v);
	if (n > 0) {
		buf.append(tmp, static_cast<std::size_t>(n));
	}
}

void JsonWriter::Uint64(uint64_t v) {
	BeforeValue();
	char tmp[32];
	auto n = std::snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
	if (n > 0) {
		buf.append(tmp, static_cast<std::size_t>(n));
	}
}

void JsonWriter::Double(double v) {
	BeforeValue();
	if (std::isnan(v)) {
		// JSON has no NaN literal; SPEC §5.4 says emit as quoted
		// string "NaN". We emit it as a JSON string (with quotes).
		buf.append("\"NaN\"", 5);
		return;
	}
	if (std::isinf(v)) {
		buf.append(v < 0 ? "\"-Infinity\"" : "\"Infinity\"");
		return;
	}
	// %.17g preserves IEEE-754 round-trip for double.
	char tmp[40];
	auto n = std::snprintf(tmp, sizeof(tmp), "%.17g", v);
	if (n > 0) {
		buf.append(tmp, static_cast<std::size_t>(n));
	}
}

void JsonWriter::String(const char *s, std::size_t len) {
	BeforeValue();
	WriteEscapedString(s, len);
}

void JsonWriter::String(const std::string &s) {
	String(s.data(), s.size());
}

void JsonWriter::RawNumber(const std::string &s) {
	BeforeValue();
	buf.append(s);
}

void JsonWriter::Raw(const std::string &raw_json_token) {
	BeforeValue();
	buf.append(raw_json_token);
}

void JsonWriter::KeyNull(const char *key) {
	Key(key);
	Null();
}
void JsonWriter::KeyBool(const char *key, bool v) {
	Key(key);
	Bool(v);
}
void JsonWriter::KeyInt64(const char *key, int64_t v) {
	Key(key);
	Int64(v);
}
void JsonWriter::KeyUint64(const char *key, uint64_t v) {
	Key(key);
	Uint64(v);
}
void JsonWriter::KeyDouble(const char *key, double v) {
	Key(key);
	Double(v);
}
void JsonWriter::KeyString(const char *key, const std::string &v) {
	Key(key);
	String(v);
}
void JsonWriter::KeyRawNumber(const char *key, const std::string &v) {
	Key(key);
	RawNumber(v);
}

void JsonWriter::WriteEscapedString(const char *s, std::size_t len) {
	buf.push_back('"');
	std::size_t i = 0;
	while (i < len) {
		auto byte = static_cast<unsigned char>(s[i]);
		// Fast-path ASCII printable (no escape needed).
		if (byte >= 0x20 && byte < 0x80 && byte != '"' && byte != '\\') {
			buf.push_back(static_cast<char>(byte));
			++i;
			continue;
		}
		// JSON-required short escapes (RFC 8259 §7).
		switch (byte) {
		case '"':
			buf.append("\\\"", 2);
			++i;
			continue;
		case '\\':
			buf.append("\\\\", 2);
			++i;
			continue;
		case '\b':
			buf.append("\\b", 2);
			++i;
			continue;
		case '\f':
			buf.append("\\f", 2);
			++i;
			continue;
		case '\n':
			buf.append("\\n", 2);
			++i;
			continue;
		case '\r':
			buf.append("\\r", 2);
			++i;
			continue;
		case '\t':
			buf.append("\\t", 2);
			++i;
			continue;
		default:
			break;
		}
		if (byte < 0x20) {
			// Other control chars: \u00XX form.
			char tmp[8];
			tmp[0] = '\\';
			tmp[1] = 'u';
			tmp[2] = '0';
			tmp[3] = '0';
			tmp[4] = kHex[(byte >> 4) & 0x0F];
			tmp[5] = kHex[byte & 0x0F];
			buf.append(tmp, 6);
			++i;
			continue;
		}
		// High byte: validate UTF-8 sequence; pass through if valid,
		// emit U+FFFD if invalid.
		auto consumed = ValidateUtf8(s, i, len);
		if (consumed == 0) {
			EmitReplacement(buf);
			i += 1; // advance one byte; resume scanning
		} else {
			buf.append(s + i, consumed);
			i += consumed;
		}
	}
	buf.push_back('"');
}

} // namespace flock_sql
} // namespace duckdb
