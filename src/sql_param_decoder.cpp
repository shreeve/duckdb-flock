// PR-5: SqlParamDecoder — JSON params → DuckDB Value per SPEC §5.2.
//
// Hand-rolled minimal JSON parser tuned for the request shape we accept:
// a top-level object with optional `sql`, `params`, `sessionId` keys;
// `params` itself is an array of bare values OR Mode B wrappers
// (`{"type":"...","value":...}`).
//
// We do NOT implement a fully RFC-conformant JSON parser. Things we
// deliberately don't handle:
//   - Number-format edge cases (we accept what `std::stod`/`stoll` accept;
//     a number with leading + is fine here even though strict JSON
//     forbids it; we still reject unambiguous garbage).
//   - Surrogate pair escape sequences `\uD83D\uDC36` in strings (we
//     decode them as separate \uXXXX codepoints; sufficient for SQL
//     parameter values which are bounded text and don't typically
//     contain emoji literal escapes).
// These could be tightened in PR-7 hardening.

#include "sql_param_decoder.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/value.hpp"

#include <cctype>
#include <cstring>
#include <utility>

namespace duckdb {
namespace flock_sql {

namespace {

bool IsHexDigit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int HexValue(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

// Encode a Unicode codepoint as UTF-8 bytes appended to `out`.
void EncodeUtf8(uint32_t cp, std::string &out) {
	if (cp < 0x80) {
		out.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

// Try to parse `text` as the integer or floating value appropriate to
// `expected_type`. Returns Value::TYPE(parsed) on success or fills
// `error` on failure.
bool CoerceNumberFromString(const std::string &text, const LogicalType &expected_type, Value &out,
                             std::string &error) {
	try {
		switch (expected_type.id()) {
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::HUGEINT:
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::UBIGINT:
		case LogicalTypeId::UHUGEINT: {
			out = Value(text).DefaultCastAs(expected_type);
			return true;
		}
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE: {
			out = Value(text).DefaultCastAs(expected_type);
			return true;
		}
		case LogicalTypeId::DECIMAL: {
			out = Value(text).DefaultCastAs(expected_type);
			return true;
		}
		default:
			error = StringUtil::Format("number cannot coerce to %s", expected_type.ToString());
			return false;
		}
	} catch (const std::exception &ex) {
		error = StringUtil::Format("number coerce to %s failed: %s", expected_type.ToString(), ex.what());
		return false;
	}
}

// Coerce a raw JSON number token (e.g. "42", "3.14", "-1e10") to the
// expected type. Integers up to 2^53 fit in double safely; we use
// CoerceNumberFromString for type-aware coercion.
bool CoerceJsonNumber(const std::string &raw, const LogicalType &expected_type, Value &out, std::string &error) {
	if (expected_type.id() == LogicalTypeId::ANY || expected_type.id() == LogicalTypeId::INVALID ||
	    expected_type.id() == LogicalTypeId::SQLNULL) {
		// SPEC §5.2 fallback: ANY/unknown → DOUBLE for fractional, BIGINT
		// for integer-shaped.
		bool is_integer = raw.find_first_of(".eE") == std::string::npos;
		try {
			if (is_integer) {
				out = Value::BIGINT(std::stoll(raw));
			} else {
				out = Value::DOUBLE(std::stod(raw));
			}
			return true;
		} catch (...) {
			error = "number out of range for ANY-typed parameter";
			return false;
		}
	}
	return CoerceNumberFromString(raw, expected_type, out, error);
}

// Coerce a JSON string to the expected type. Strings can target many
// DuckDB types (DECIMAL, UUID, DATE, TIMESTAMP, INTERVAL, BLOB-base64,
// JSON, plus VARCHAR which is the trivial case).
bool CoerceJsonString(const std::string &text, const LogicalType &expected_type, Value &out, std::string &error) {
	try {
		switch (expected_type.id()) {
		case LogicalTypeId::VARCHAR:
			out = Value(text);
			return true;
		case LogicalTypeId::ANY:
		case LogicalTypeId::INVALID:
			out = Value(text);
			return true;
		case LogicalTypeId::BLOB: {
			// SPEC §5.4 says BLOB is base64; param decode mirrors that.
			Value blob_val(text);
			out = blob_val.DefaultCastAs(LogicalType::BLOB);
			return true;
		}
		default:
			// Pass through DuckDB's string-cast machinery; handles
			// UUID, DATE, TIME, TIMESTAMP, INTERVAL, DECIMAL (string
			// form), JSON, ENUM (label string).
			out = Value(text).DefaultCastAs(expected_type);
			return true;
		}
	} catch (const std::exception &ex) {
		error =
		    StringUtil::Format("string parameter cannot coerce to %s: %s", expected_type.ToString(), ex.what());
		return false;
	}
}

bool ParseSimpleLogicalType(const std::string &input, LogicalType &out, std::string &error) {
	auto s = StringUtil::Upper(input);
	StringUtil::Trim(s);
	if (s == "BOOLEAN") out = LogicalType::BOOLEAN;
	else if (s == "TINYINT") out = LogicalType::TINYINT;
	else if (s == "SMALLINT") out = LogicalType::SMALLINT;
	else if (s == "INTEGER" || s == "INT") out = LogicalType::INTEGER;
	else if (s == "BIGINT") out = LogicalType::BIGINT;
	else if (s == "HUGEINT") out = LogicalType::HUGEINT;
	else if (s == "UTINYINT") out = LogicalType::UTINYINT;
	else if (s == "USMALLINT") out = LogicalType::USMALLINT;
	else if (s == "UINTEGER") out = LogicalType::UINTEGER;
	else if (s == "UBIGINT") out = LogicalType::UBIGINT;
	else if (s == "UHUGEINT") out = LogicalType::UHUGEINT;
	else if (s == "FLOAT") out = LogicalType::FLOAT;
	else if (s == "DOUBLE") out = LogicalType::DOUBLE;
	else if (s == "VARCHAR" || s == "STRING") out = LogicalType::VARCHAR;
	else if (s == "UUID") out = LogicalType::UUID;
	else if (s == "DATE") out = LogicalType::DATE;
	else if (s == "TIME") out = LogicalType::TIME;
	else if (s == "TIMETZ" || s == "TIME WITH TIME ZONE") out = LogicalType::TIME_TZ;
	else if (s == "TIMESTAMP") out = LogicalType::TIMESTAMP;
	else if (s == "TIMESTAMP_S") out = LogicalType::TIMESTAMP_S;
	else if (s == "TIMESTAMP_MS") out = LogicalType::TIMESTAMP_MS;
	else if (s == "TIMESTAMP_NS") out = LogicalType::TIMESTAMP_NS;
	else if (s == "TIMESTAMPTZ" || s == "TIMESTAMP WITH TIME ZONE") out = LogicalType::TIMESTAMP_TZ;
	else if (s == "INTERVAL") out = LogicalType::INTERVAL;
	else if (s == "BLOB") out = LogicalType::BLOB;
	else if (s == "BIT") out = LogicalType::BIT;
	else if (StringUtil::StartsWith(s, "DECIMAL(")) {
		auto l = s.find('(');
		auto comma = s.find(',', l + 1);
		auto r = s.find(')', comma + 1);
		if (l == std::string::npos || comma == std::string::npos || r == std::string::npos) {
			error = "DECIMAL type must be DECIMAL(width,scale)";
			return false;
		}
		try {
			auto width = static_cast<uint8_t>(std::stoul(s.substr(l + 1, comma - l - 1)));
			auto scale = static_cast<uint8_t>(std::stoul(s.substr(comma + 1, r - comma - 1)));
			out = LogicalType::DECIMAL(width, scale);
			return true;
		} catch (...) {
			error = "DECIMAL width/scale must be integers";
			return false;
		}
	} else {
		error = StringUtil::Format("unsupported Mode B type '%s' (PR-5 supports core scalar type strings; nested typed wrappers are PR-7 hardening)", input);
		return false;
	}
	return true;
}

} // namespace

// ---------- low-level JSON tokenization ----------

void SqlParamDecoder::SkipWhitespace(const std::string &json, std::size_t &pos) {
	while (pos < json.size()) {
		char c = json[pos];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			++pos;
		} else {
			break;
		}
	}
}

bool SqlParamDecoder::ConsumeChar(const std::string &json, std::size_t &pos, char expected, std::string &error) {
	SkipWhitespace(json, pos);
	if (pos >= json.size() || json[pos] != expected) {
		error = StringUtil::Format("expected '%c' at byte %llu", expected, (unsigned long long)pos);
		return false;
	}
	++pos;
	return true;
}

bool SqlParamDecoder::ParseString(const std::string &json, std::size_t &pos, std::string &out,
                                    std::string &error) {
	SkipWhitespace(json, pos);
	if (pos >= json.size() || json[pos] != '"') {
		error = "expected JSON string";
		return false;
	}
	++pos;
	out.clear();
	while (pos < json.size()) {
		char c = json[pos];
		if (c == '"') {
			++pos;
			return true;
		}
		if (c == '\\') {
			if (pos + 1 >= json.size()) {
				error = "unterminated escape";
				return false;
			}
			char esc = json[pos + 1];
			pos += 2;
			switch (esc) {
			case '"':
				out.push_back('"');
				break;
			case '\\':
				out.push_back('\\');
				break;
			case '/':
				out.push_back('/');
				break;
			case 'b':
				out.push_back('\b');
				break;
			case 'f':
				out.push_back('\f');
				break;
			case 'n':
				out.push_back('\n');
				break;
			case 'r':
				out.push_back('\r');
				break;
			case 't':
				out.push_back('\t');
				break;
			case 'u': {
				if (pos + 4 > json.size()) {
					error = "truncated \\u escape";
					return false;
				}
				uint32_t cp = 0;
				for (int i = 0; i < 4; i++) {
					int v = HexValue(json[pos + i]);
					if (v < 0) {
						error = "invalid hex in \\u escape";
						return false;
					}
					cp = (cp << 4) | static_cast<uint32_t>(v);
				}
				pos += 4;
				EncodeUtf8(cp, out);
				break;
			}
			default:
				error = StringUtil::Format("unknown escape \\%c", esc);
				return false;
			}
			continue;
		}
		out.push_back(c);
		++pos;
	}
	error = "unterminated string";
	return false;
}

bool SqlParamDecoder::ParseRawValue(const std::string &json, std::size_t &pos, std::string &raw_out,
                                      std::string &error) {
	SkipWhitespace(json, pos);
	if (pos >= json.size()) {
		error = "unexpected end of input";
		return false;
	}
	auto start = pos;
	char c = json[pos];

	// String — find matching close-quote (handle escapes).
	if (c == '"') {
		++pos;
		while (pos < json.size()) {
			if (json[pos] == '\\' && pos + 1 < json.size()) {
				pos += 2;
				continue;
			}
			if (json[pos] == '"') {
				++pos;
				raw_out = json.substr(start, pos - start);
				return true;
			}
			++pos;
		}
		error = "unterminated string in raw-value scan";
		return false;
	}
	// Object or array — bracket-counting scan.
	if (c == '{' || c == '[') {
		char open_ch = c;
		char close_ch = (c == '{') ? '}' : ']';
		int depth = 0;
		bool in_string = false;
		while (pos < json.size()) {
			c = json[pos];
			if (in_string) {
				if (c == '\\' && pos + 1 < json.size()) {
					pos += 2;
					continue;
				}
				if (c == '"') {
					in_string = false;
				}
				++pos;
				continue;
			}
			if (c == '"') {
				in_string = true;
				++pos;
				continue;
			}
			if (c == open_ch) {
				++depth;
				++pos;
				continue;
			}
			if (c == close_ch) {
				--depth;
				++pos;
				if (depth == 0) {
					raw_out = json.substr(start, pos - start);
					return true;
				}
				continue;
			}
			++pos;
		}
		error = "unterminated object/array";
		return false;
	}
	// Literal (true/false/null) or number.
	while (pos < json.size()) {
		char cc = json[pos];
		if (cc == ',' || cc == ']' || cc == '}' || cc == ' ' || cc == '\t' || cc == '\n' || cc == '\r') {
			break;
		}
		++pos;
	}
	if (pos == start) {
		error = "expected value";
		return false;
	}
	raw_out = json.substr(start, pos - start);
	return true;
}

// ---------- top-level request parser ----------

SqlParamDecoder::ParsedRequest SqlParamDecoder::ParseRequest(const std::string &body) {
	ParsedRequest result;
	std::size_t pos = 0;
	SqlParamDecoder dec; // for SkipWhitespace/ConsumeChar/ParseString
	dec.SkipWhitespace(body, pos);
	if (pos >= body.size() || body[pos] != '{') {
		result.ok = false;
		result.error = "request body must be a JSON object";
		return result;
	}
	++pos;
	bool first = true;
	while (true) {
		dec.SkipWhitespace(body, pos);
		if (pos < body.size() && body[pos] == '}') {
			++pos;
			break;
		}
		if (!first) {
			if (!dec.ConsumeChar(body, pos, ',', result.error)) {
				result.ok = false;
				return result;
			}
			dec.SkipWhitespace(body, pos);
		}
		first = false;
		std::string key;
		if (!dec.ParseString(body, pos, key, result.error)) {
			result.ok = false;
			return result;
		}
		if (!dec.ConsumeChar(body, pos, ':', result.error)) {
			result.ok = false;
			return result;
		}
		std::string raw;
		if (!dec.ParseRawValue(body, pos, raw, result.error)) {
			result.ok = false;
			return result;
		}
		// Dispatch known keys; ignore unknown.
		if (key == "sql") {
			std::string s;
			std::size_t inner_pos = 0;
			if (!dec.ParseString(raw, inner_pos, s, result.error)) {
				result.ok = false;
				return result;
			}
			result.sql = std::move(s);
		} else if (key == "sessionId") {
			std::string s;
			std::size_t inner_pos = 0;
			if (!dec.ParseString(raw, inner_pos, s, result.error)) {
				result.ok = false;
				return result;
			}
			result.session_id = std::move(s);
		} else if (key == "params") {
			result.params_json = std::move(raw);
		}
	}
	if (result.sql.empty()) {
		result.ok = false;
		result.error = "request missing required `sql` field";
	}
	return result;
}

// ---------- params decoding ----------

bool SqlParamDecoder::DecodeOne(const std::string &json, std::size_t &pos, const LogicalType &expected_type,
                                  Value &out, ClientContext &context, std::string &error) {
	SkipWhitespace(json, pos);
	if (pos >= json.size()) {
		error = "expected value";
		return false;
	}
	char c = json[pos];

	// JSON null → NULL of expected type.
	if (c == 'n') {
		if (json.compare(pos, 4, "null") == 0) {
			pos += 4;
			out = Value(expected_type);
			return true;
		}
		error = "expected `null`";
		return false;
	}
	// JSON true/false → BOOLEAN.
	if (c == 't' || c == 'f') {
		bool bv = (c == 't');
		const char *lit = bv ? "true" : "false";
		std::size_t len = bv ? 4 : 5;
		if (json.compare(pos, len, lit) != 0) {
			error = "expected boolean literal";
			return false;
		}
		pos += len;
		try {
			out = Value::BOOLEAN(bv);
			if (expected_type.id() != LogicalTypeId::BOOLEAN && expected_type.id() != LogicalTypeId::ANY &&
			    expected_type.id() != LogicalTypeId::INVALID) {
				out = out.DefaultCastAs(expected_type);
			}
			return true;
		} catch (const std::exception &ex) {
			error = StringUtil::Format("boolean cannot coerce to %s: %s", expected_type.ToString(), ex.what());
			return false;
		}
	}
	// String.
	if (c == '"') {
		std::string s;
		if (!ParseString(json, pos, s, error)) {
			return false;
		}
		return CoerceJsonString(s, expected_type, out, error);
	}
	// Number.
	if (c == '-' || (c >= '0' && c <= '9')) {
		auto start = pos;
		// Crude number scan: digits, sign, dot, eE.
		while (pos < json.size()) {
			char cc = json[pos];
			if ((cc >= '0' && cc <= '9') || cc == '-' || cc == '+' || cc == '.' || cc == 'e' || cc == 'E') {
				++pos;
			} else {
				break;
			}
		}
		auto raw = json.substr(start, pos - start);
		return CoerceJsonNumber(raw, expected_type, out, error);
	}
	// Object — Mode B wrapper OR Mode A STRUCT.
	if (c == '{') {
		// Detect Mode B wrapper. JSON object order is not significant,
		// so accept both {"type": "...", "value": ...} and
		// {"value": ..., "type": "..."} (round-16 GPT-5.5 catch).
		auto saved = pos;
		std::string raw_object;
		std::string ignored_err;
		if (ParseRawValue(json, pos, raw_object, ignored_err) &&
		    raw_object.find("\"type\"") != std::string::npos &&
		    raw_object.find("\"value\"") != std::string::npos) {
			pos = saved;
			return DecodeTypedWrapper(json, pos, out, context, error);
		}
		// Reset; treat as Mode A STRUCT.
		pos = saved;
		if (expected_type.id() != LogicalTypeId::STRUCT) {
			error =
			    StringUtil::Format("object value cannot coerce to %s (use Mode B {type, value} for non-STRUCT)",
			                        expected_type.ToString());
			return false;
		}
		// Parse {key: value, ...} and assemble into a STRUCT Value
		// using the expected struct's child types.
		auto &child_types = StructType::GetChildTypes(expected_type);
		child_list_t<Value> struct_values;
		if (!ConsumeChar(json, pos, '{', error)) {
			return false;
		}
		bool first = true;
		while (true) {
			SkipWhitespace(json, pos);
			if (pos < json.size() && json[pos] == '}') {
				++pos;
				break;
			}
			if (!first) {
				if (!ConsumeChar(json, pos, ',', error)) {
					return false;
				}
			}
			first = false;
			std::string key;
			if (!ParseString(json, pos, key, error)) {
				return false;
			}
			if (!ConsumeChar(json, pos, ':', error)) {
				return false;
			}
			// Find expected child type by key name.
			LogicalType child_type = LogicalType::ANY;
			bool found = false;
			for (auto &ct : child_types) {
				if (ct.first == key) {
					child_type = ct.second;
					found = true;
					break;
				}
			}
			if (!found) {
				error = StringUtil::Format("STRUCT field '%s' not in expected type %s", key,
				                            expected_type.ToString());
				return false;
			}
			Value child_val;
			if (!DecodeOne(json, pos, child_type, child_val, context, error)) {
				return false;
			}
			struct_values.emplace_back(key, std::move(child_val));
		}
		try {
			out = Value::STRUCT(std::move(struct_values));
			out = out.DefaultCastAs(expected_type);
			return true;
		} catch (const std::exception &ex) {
			error = StringUtil::Format("STRUCT assembly failed: %s", ex.what());
			return false;
		}
	}
	// Array — Mode A LIST or ARRAY.
	if (c == '[') {
		LogicalType child_type;
		bool is_array = false;
		idx_t array_size = 0;
		if (expected_type.id() == LogicalTypeId::LIST) {
			child_type = ListType::GetChildType(expected_type);
		} else if (expected_type.id() == LogicalTypeId::ARRAY) {
			child_type = ArrayType::GetChildType(expected_type);
			array_size = ArrayType::GetSize(expected_type);
			is_array = true;
		} else {
			error = StringUtil::Format(
			    "array value cannot coerce to %s (expected LIST or ARRAY; or use Mode B for MAP)",
			    expected_type.ToString());
			return false;
		}
		if (!ConsumeChar(json, pos, '[', error)) {
			return false;
		}
		std::vector<Value> elements; // local accumulator; converted to duckdb::vector before Value::LIST/ARRAY
		bool first = true;
		while (true) {
			SkipWhitespace(json, pos);
			if (pos < json.size() && json[pos] == ']') {
				++pos;
				break;
			}
			if (!first) {
				if (!ConsumeChar(json, pos, ',', error)) {
					return false;
				}
			}
			first = false;
			Value elem;
			if (!DecodeOne(json, pos, child_type, elem, context, error)) {
				return false;
			}
			elements.push_back(std::move(elem));
		}
		try {
			// Convert std::vector to duckdb::vector for Value::ARRAY/LIST
			// (same subclass-vs-base constraint as the params vector).
			vector<Value> elements_vec(std::make_move_iterator(elements.begin()),
			                            std::make_move_iterator(elements.end()));
			if (is_array) {
				if (elements_vec.size() != array_size) {
					error = StringUtil::Format("ARRAY length mismatch (got %zu, expected %llu)",
					                            elements_vec.size(), (unsigned long long)array_size);
					return false;
				}
				out = Value::ARRAY(child_type, std::move(elements_vec));
			} else {
				out = Value::LIST(child_type, std::move(elements_vec));
			}
			return true;
		} catch (const std::exception &ex) {
			error = StringUtil::Format("LIST/ARRAY assembly failed: %s", ex.what());
			return false;
		}
	}
	error = StringUtil::Format("unexpected character '%c' at byte %llu", c, (unsigned long long)pos);
	return false;
}

bool SqlParamDecoder::DecodeTypedWrapper(const std::string &json, std::size_t &pos, Value &out,
                                          ClientContext &context, std::string &error) {
	if (!ConsumeChar(json, pos, '{', error)) {
		return false;
	}
	// Parse {"type": "...", "value": ...}. Strict order is allowed
	// either way (type-first or value-first) per JSON object semantics;
	// we accept both.
	std::string type_str;
	std::string value_raw;
	bool have_type = false;
	bool have_value = false;
	bool first = true;
	while (true) {
		SkipWhitespace(json, pos);
		if (pos < json.size() && json[pos] == '}') {
			++pos;
			break;
		}
		if (!first) {
			if (!ConsumeChar(json, pos, ',', error)) {
				return false;
			}
		}
		first = false;
		std::string key;
		if (!ParseString(json, pos, key, error)) {
			return false;
		}
		if (!ConsumeChar(json, pos, ':', error)) {
			return false;
		}
		if (key == "type") {
			std::string s;
			if (!ParseString(json, pos, s, error)) {
				return false;
			}
			type_str = std::move(s);
			have_type = true;
		} else if (key == "value") {
			if (!ParseRawValue(json, pos, value_raw, error)) {
				return false;
			}
			have_value = true;
		} else {
			// Skip unknown key (forward-compat).
			std::string ignored;
			if (!ParseRawValue(json, pos, ignored, error)) {
				return false;
			}
		}
	}
	if (!have_type) {
		error = "Mode B wrapper missing `type`";
		return false;
	}
	if (!have_value) {
		error = "Mode B wrapper missing `value`";
		return false;
	}
	// Parse type_str. Avoid TransformStringToLogicalType here: in
	// v1.5.x it requires an active ClientContext transaction, which is
	// awkward inside the parameter decoder and caused an INTERNAL
	// exception for simple DECIMAL(18,4). PR-5 supports the core scalar
	// type strings needed by SPEC §5.2. Nested wrapper type parsing can
	// be tightened in PR-7 hardening if needed.
	LogicalType wrapped_type;
	if (!ParseSimpleLogicalType(type_str, wrapped_type, error)) {
		error = StringUtil::Format("Mode B `type` not recognized: %s (%s)", type_str, error);
		return false;
	}
	// Recursive: decode the wrapped value AS the wrapped type.
	std::size_t inner_pos = 0;
	if (!DecodeOne(value_raw, inner_pos, wrapped_type, out, context, error)) {
		return false;
	}
	return true;
}

DecodedParams SqlParamDecoder::Decode(const std::string &params_json,
                                        const vector<LogicalType> &expected_types,
                                        ClientContext &context) {
	DecodedParams result;
	auto trimmed = params_json;
	while (!trimmed.empty() &&
	       (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\n' || trimmed.back() == '\r')) {
		trimmed.pop_back();
	}
	std::size_t lead = 0;
	while (lead < trimmed.size() &&
	       (trimmed[lead] == ' ' || trimmed[lead] == '\t' || trimmed[lead] == '\n' || trimmed[lead] == '\r')) {
		++lead;
	}
	if (lead) {
		trimmed = trimmed.substr(lead);
	}

	if (trimmed.empty() || trimmed == "[]") {
		if (!expected_types.empty()) {
			result.ok = false;
			result.error =
			    StringUtil::Format("prepared statement expects %zu parameter(s); none provided", expected_types.size());
		}
		return result;
	}

	std::size_t pos = 0;
	if (!ConsumeChar(trimmed, pos, '[', result.error)) {
		result.ok = false;
		return result;
	}
	idx_t idx = 0;
	bool first = true;
	while (true) {
		SkipWhitespace(trimmed, pos);
		if (pos < trimmed.size() && trimmed[pos] == ']') {
			++pos;
			break;
		}
		if (!first) {
			if (!ConsumeChar(trimmed, pos, ',', result.error)) {
				result.ok = false;
				return result;
			}
		}
		first = false;
		if (!expected_types.empty() && idx >= expected_types.size()) {
			result.ok = false;
			result.error = StringUtil::Format("more parameters than prepared statement expects (%zu)",
			                                  expected_types.size());
			return result;
		}
		Value v;
		auto expected_type = expected_types.empty() ? LogicalType::ANY : expected_types[idx];
		if (!DecodeOne(trimmed, pos, expected_type, v, context, result.error)) {
			result.ok = false;
			return result;
		}
		result.values.push_back(std::move(v));
		++idx;
	}
	if (!expected_types.empty() && idx != expected_types.size()) {
		result.ok = false;
		result.error = StringUtil::Format("prepared statement expects %zu parameter(s); got %llu",
		                                  expected_types.size(), (unsigned long long)idx);
	}
	return result;
}

} // namespace flock_sql
} // namespace duckdb
