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
namespace harbor_sql {

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

// PR-7d — recursion safety bounds. Per round-25 review: type strings
// are bounded by request body cap already, but a local cap gives
// clearer errors and prevents pathological type strings from blowing
// the C stack via deep recursion. 32 nesting levels and 4 KiB type
// string are far above any realistic Mode B usage.
constexpr int kMaxTypeNesting = 32;
constexpr std::size_t kMaxTypeStringLength = 4096;

// PR-7d — find the matching `>` or `)` for the bracket character at
// `start_idx`, ignoring brackets inside quoted strings (we don't
// support quoted field names in v0.1 type strings — round-25 — but
// the helper is safe in case future types have them). Returns the
// matching index, or std::string::npos if unbalanced.
//
// `open` is the bracket at start_idx (e.g. `<`). `close` is the
// matching close bracket (e.g. `>`). Counts nested `< >` AND `( )`
// so DECIMAL(10,2) inside LIST<DECIMAL(10,2)> doesn't unbalance.
std::size_t FindMatchingBracket(const std::string &s, std::size_t start_idx, char open, char close) {
	int depth = 0;
	for (std::size_t i = start_idx; i < s.size(); ++i) {
		const char c = s[i];
		if (c == '(' || c == '<') {
			++depth;
		} else if (c == ')' || c == '>') {
			--depth;
			if (depth == 0 && c == close) {
				return i;
			}
		}
	}
	return std::string::npos;
}

// PR-7d — split `s` on top-level commas, respecting nested `< >` and
// `( )`. Returns a list of trimmed substrings. Used by STRUCT(...)
// field parsing and MAP<K,V> / ARRAY<T,N> argument parsing.
std::vector<std::string> SplitTopLevelCommas(const std::string &s) {
	std::vector<std::string> out;
	int angle_depth = 0;
	int paren_depth = 0;
	std::size_t segment_start = 0;
	for (std::size_t i = 0; i < s.size(); ++i) {
		const char c = s[i];
		if (c == '<') {
			++angle_depth;
		} else if (c == '>') {
			--angle_depth;
		} else if (c == '(') {
			++paren_depth;
		} else if (c == ')') {
			--paren_depth;
		} else if (c == ',' && angle_depth == 0 && paren_depth == 0) {
			std::string seg = s.substr(segment_start, i - segment_start);
			StringUtil::Trim(seg);
			out.push_back(std::move(seg));
			segment_start = i + 1;
		}
	}
	if (segment_start < s.size()) {
		std::string seg = s.substr(segment_start);
		StringUtil::Trim(seg);
		out.push_back(std::move(seg));
	}
	return out;
}

// Forward declaration; defined below ParseSimpleLogicalType.
bool ParseLogicalType(const std::string &input, LogicalType &out, std::string &error, int depth);

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
		error = StringUtil::Format("unsupported Mode B scalar type '%s'", input);
		return false;
	}
	return true;
}

// PR-7d — recursive-descent parser for the documented Mode B nested
// type grammar (round-25 review). Supports:
//
//   LIST<T>                        — variadic-length list of T
//   ARRAY<T, N>                    — fixed-size N array of T
//   MAP<K, V>                      — map from K to V (K, V are types)
//   STRUCT(name1 type1, name2 type2, ...)   — named-field struct
//
// Defers to ParseSimpleLogicalType for scalars + DECIMAL.
//
// NOT supported in v0.1:
//   - UNION(name1 type1, ...)      — operator can model as STRUCT
//   - DuckDB shorthand `T[]`, `INTEGER[3]` — clients use the LIST/ARRAY forms
//   - Quoted struct field names    — only `[a-zA-Z_][a-zA-Z0-9_]*`
//   - User-defined types via       — Mode A expected-type inference
//     extension catalog              handles those when the prepared
//                                    statement knows the type
//
// Error messages stay client-actionable: "MAP<K,V> requires two
// type arguments" rather than DuckDB internal exceptions.
bool ParseLogicalType(const std::string &input, LogicalType &out, std::string &error, int depth) {
	if (depth > kMaxTypeNesting) {
		error = StringUtil::Format("type string nested more than %d levels deep", kMaxTypeNesting);
		return false;
	}
	if (input.size() > kMaxTypeStringLength) {
		error = StringUtil::Format("type string longer than %llu bytes", (unsigned long long)kMaxTypeStringLength);
		return false;
	}
	// PR-7d (round-25): normalize whitespace before parsing. Collapse
	// runs of whitespace to a single space, AND drop whitespace
	// adjacent to brackets/parens/commas. So `LIST < INTEGER >` and
	// `STRUCT( a INTEGER ,  b VARCHAR )` and `MAP<K , V>` all parse
	// the same as the canonical `LIST<INTEGER>` / `STRUCT(a INTEGER,b VARCHAR)`
	// / `MAP<K,V>` forms. The single-space-preserved-between-non-special
	// rule keeps the `name TYPE` separator intact in STRUCT field
	// declarations.
	std::string s;
	s.reserve(input.size());
	auto is_special = [](char c) {
		return c == '<' || c == '>' || c == '(' || c == ')' || c == ',';
	};
	{
		std::size_t i = 0;
		// Skip leading whitespace.
		while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i]))) {
			++i;
		}
		while (i < input.size()) {
			char c = input[i];
			if (std::isspace(static_cast<unsigned char>(c))) {
				// Scan ahead to next non-whitespace character.
				std::size_t j = i + 1;
				while (j < input.size() && std::isspace(static_cast<unsigned char>(input[j]))) {
					++j;
				}
				const bool prev_is_special = !s.empty() && is_special(s.back());
				const bool next_is_special = j < input.size() && is_special(input[j]);
				if (!prev_is_special && !next_is_special && j < input.size()) {
					s.push_back(' ');
				}
				i = j;
			} else {
				s.push_back(c);
				++i;
			}
		}
	}
	if (s.empty()) {
		error = "empty type string";
		return false;
	}
	std::string upper = StringUtil::Upper(s);

	auto parse_bracketed_args = [&](const char *name, std::size_t header_len, char open, char close,
	                                 std::string &inner_out) -> bool {
		// `s` looks like "LIST<...>" or "STRUCT(...)". `header_len` is
		// the byte count of the leading keyword excluding the open
		// bracket. Find the matching close, extract the inner substring.
		if (s.size() < header_len + 2) {
			error = StringUtil::Format("%s missing arguments", name);
			return false;
		}
		auto open_idx = header_len;
		if (s[open_idx] != open) {
			error = StringUtil::Format("%s expected '%c' after keyword", name, open);
			return false;
		}
		auto close_idx = FindMatchingBracket(s, open_idx, open, close);
		if (close_idx == std::string::npos) {
			error = StringUtil::Format("%s unbalanced '%c'/'%c'", name, open, close);
			return false;
		}
		// Trailing content after the close bracket is an error
		// (no `LIST<INTEGER> garbage`).
		std::size_t after = close_idx + 1;
		while (after < s.size() && std::isspace(static_cast<unsigned char>(s[after]))) {
			++after;
		}
		if (after != s.size()) {
			error = StringUtil::Format("%s has trailing garbage after closing bracket", name);
			return false;
		}
		inner_out = s.substr(open_idx + 1, close_idx - open_idx - 1);
		return true;
	};

	if (StringUtil::StartsWith(upper, "LIST<")) {
		std::string inner;
		if (!parse_bracketed_args("LIST<...>", 4, '<', '>', inner)) {
			return false;
		}
		LogicalType child;
		if (!ParseLogicalType(inner, child, error, depth + 1)) {
			return false;
		}
		out = LogicalType::LIST(child);
		return true;
	}
	if (StringUtil::StartsWith(upper, "ARRAY<")) {
		std::string inner;
		if (!parse_bracketed_args("ARRAY<T,N>", 5, '<', '>', inner)) {
			return false;
		}
		auto parts = SplitTopLevelCommas(inner);
		if (parts.size() != 2) {
			error = "ARRAY<T,N> requires exactly two arguments (element type and length)";
			return false;
		}
		LogicalType child;
		if (!ParseLogicalType(parts[0], child, error, depth + 1)) {
			return false;
		}
		try {
			auto n = std::stoul(parts[1]);
			if (n == 0) {
				error = "ARRAY<T,N> length must be > 0";
				return false;
			}
			out = LogicalType::ARRAY(child, static_cast<idx_t>(n));
			return true;
		} catch (...) {
			error = StringUtil::Format("ARRAY<T,N> length '%s' is not a positive integer", parts[1]);
			return false;
		}
	}
	if (StringUtil::StartsWith(upper, "MAP<")) {
		std::string inner;
		if (!parse_bracketed_args("MAP<K,V>", 3, '<', '>', inner)) {
			return false;
		}
		auto parts = SplitTopLevelCommas(inner);
		if (parts.size() != 2) {
			error = "MAP<K,V> requires exactly two type arguments (key and value)";
			return false;
		}
		LogicalType key, val;
		if (!ParseLogicalType(parts[0], key, error, depth + 1)) {
			return false;
		}
		if (!ParseLogicalType(parts[1], val, error, depth + 1)) {
			return false;
		}
		out = LogicalType::MAP(key, val);
		return true;
	}
	if (StringUtil::StartsWith(upper, "STRUCT(")) {
		std::string inner;
		if (!parse_bracketed_args("STRUCT(...)", 6, '(', ')', inner)) {
			return false;
		}
		auto fields = SplitTopLevelCommas(inner);
		if (fields.empty()) {
			error = "STRUCT(...) requires at least one field";
			return false;
		}
		child_list_t<LogicalType> children;
		for (auto &field : fields) {
			// Each field is `name type`. Split on first whitespace.
			auto sp = field.find_first_of(" \t");
			if (sp == std::string::npos) {
				error = StringUtil::Format("STRUCT field '%s' missing type", field);
				return false;
			}
			std::string name = field.substr(0, sp);
			std::string type_part = field.substr(sp + 1);
			StringUtil::Trim(name);
			StringUtil::Trim(type_part);
			// Validate name: simple identifier `[a-zA-Z_][a-zA-Z0-9_]*` per round-25.
			if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
				error = StringUtil::Format("STRUCT field name '%s' must match [a-zA-Z_][a-zA-Z0-9_]*", name);
				return false;
			}
			for (char c : name) {
				if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
					error = StringUtil::Format("STRUCT field name '%s' contains invalid character", name);
					return false;
				}
			}
			LogicalType field_type;
			if (!ParseLogicalType(type_part, field_type, error, depth + 1)) {
				return false;
			}
			children.emplace_back(std::move(name), std::move(field_type));
		}
		out = LogicalType::STRUCT(std::move(children));
		return true;
	}
	if (StringUtil::StartsWith(upper, "UNION(")) {
		// Round-25: explicit unsupported error (better than "unrecognized type").
		error = "UNION(...) is not supported in Mode B for v0.1; use STRUCT to model tagged variants";
		return false;
	}

	// Fall through to scalars + DECIMAL.
	return ParseSimpleLogicalType(s, out, error);
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
		// PR-7d (round-25): case-insensitive STRUCT field lookup with
		// duplicate-key rejection and missing-field-as-NULL semantics.
		// Per the round-25 review:
		//   - Parsing the type STRING preserves field name as written.
		//   - Decoding the JSON object uses case-insensitive match
		//     (DuckDB SQL identifier convention).
		//   - Duplicate JSON keys that differ only in case (e.g.
		//     {"A":1,"a":2}) are rejected as BAD_REQUEST — arbitrary
		//     winner would be a footgun.
		//   - Extra JSON keys not in the expected struct are rejected.
		//   - Missing struct fields are NULL.
		auto &child_types = StructType::GetChildTypes(expected_type);
		child_list_t<Value> struct_values;
		// Track which fields we've populated, indexed into child_types,
		// so duplicate-input-key (case-insensitive) detection is O(1) per
		// key and missing fields can be backfilled with NULL.
		std::vector<bool> seen(child_types.size(), false);
		std::vector<Value> populated(child_types.size());
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
			// Case-insensitive find against the expected child types.
			std::string key_lower = StringUtil::Lower(key);
			std::size_t match_idx = child_types.size();
			for (std::size_t i = 0; i < child_types.size(); ++i) {
				if (StringUtil::Lower(child_types[i].first) == key_lower) {
					match_idx = i;
					break;
				}
			}
			if (match_idx == child_types.size()) {
				error = StringUtil::Format("STRUCT field '%s' not in expected type %s", key,
				                            expected_type.ToString());
				return false;
			}
			if (seen[match_idx]) {
				// Round-25: duplicate key (case-insensitive) is BAD_REQUEST,
				// not arbitrary winner.
				error = StringUtil::Format(
				    "STRUCT input has duplicate field '%s' (case-insensitive collision with '%s')",
				    key, child_types[match_idx].first);
				return false;
			}
			Value child_val;
			if (!DecodeOne(json, pos, child_types[match_idx].second, child_val, context, error)) {
				return false;
			}
			seen[match_idx] = true;
			populated[match_idx] = std::move(child_val);
		}
		// Backfill missing fields as NULL (round-25: missing → NULL,
		// extra → BAD_REQUEST).
		for (std::size_t i = 0; i < child_types.size(); ++i) {
			if (seen[i]) {
				struct_values.emplace_back(child_types[i].first, std::move(populated[i]));
			} else {
				struct_values.emplace_back(child_types[i].first, Value(child_types[i].second));
			}
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
	// Array — LIST / ARRAY / MAP. PR-7d (round-25): MAP value shape
	// is array-of-pairs `[[k,v],[k,v],...]` (NOT a JSON object;
	// non-string keys would be ambiguous as object keys, and MAP
	// allows duplicate keys per DuckDB semantics).
	if (c == '[') {
		LogicalType child_type;
		bool is_array = false;
		bool is_map = false;
		idx_t array_size = 0;
		LogicalType map_key_type;
		LogicalType map_value_type;
		if (expected_type.id() == LogicalTypeId::LIST) {
			child_type = ListType::GetChildType(expected_type);
		} else if (expected_type.id() == LogicalTypeId::ARRAY) {
			child_type = ArrayType::GetChildType(expected_type);
			array_size = ArrayType::GetSize(expected_type);
			is_array = true;
		} else if (expected_type.id() == LogicalTypeId::MAP) {
			// MAP<K,V> internally is LIST<STRUCT(key K, value V)>.
			// We synthesize a child STRUCT type from each pair.
			map_key_type = MapType::KeyType(expected_type);
			map_value_type = MapType::ValueType(expected_type);
			is_map = true;
		} else {
			error = StringUtil::Format(
			    "array value cannot coerce to %s (expected LIST, ARRAY, or MAP)",
			    expected_type.ToString());
			return false;
		}
		if (!ConsumeChar(json, pos, '[', error)) {
			return false;
		}
		std::vector<Value> elements; // local accumulator; converted to duckdb::vector before Value::LIST/ARRAY/MAP
		// For MAP we accumulate (key, value) pairs separately so we can
		// assemble Value::MAP at the end with the right child types.
		std::vector<Value> map_keys;
		std::vector<Value> map_values;
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
			if (is_map) {
				// Each map element is a 2-element pair `[K, V]`.
				if (!ConsumeChar(json, pos, '[', error)) {
					error = "MAP element must be a 2-element [key, value] pair";
					return false;
				}
				Value k, v;
				if (!DecodeOne(json, pos, map_key_type, k, context, error)) {
					return false;
				}
				if (!ConsumeChar(json, pos, ',', error)) {
					return false;
				}
				if (!DecodeOne(json, pos, map_value_type, v, context, error)) {
					return false;
				}
				if (!ConsumeChar(json, pos, ']', error)) {
					error = "MAP element must be exactly [key, value] (extra entries rejected)";
					return false;
				}
				map_keys.push_back(std::move(k));
				map_values.push_back(std::move(v));
				continue;
			}
			Value elem;
			if (!DecodeOne(json, pos, child_type, elem, context, error)) {
				return false;
			}
			elements.push_back(std::move(elem));
		}
		try {
			if (is_map) {
				vector<Value> keys_vec(std::make_move_iterator(map_keys.begin()),
				                        std::make_move_iterator(map_keys.end()));
				vector<Value> values_vec(std::make_move_iterator(map_values.begin()),
				                          std::make_move_iterator(map_values.end()));
				out = Value::MAP(map_key_type, map_value_type, std::move(keys_vec), std::move(values_vec));
				return true;
			}
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
			error = StringUtil::Format("LIST/ARRAY/MAP assembly failed: %s", ex.what());
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
	// PR-7d (round-25): full nested-type Mode B grammar supported by
	// the recursive ParseLogicalType. Handles LIST<T>, ARRAY<T,N>,
	// MAP<K,V>, STRUCT(name1 type1, ...), and all PR-5 scalar types
	// + DECIMAL(width,scale). Avoids TransformStringToLogicalType
	// (PR-5 history: that requires an active ClientContext transaction
	// and caused an INTERNAL exception for simple DECIMAL(18,4)).
	LogicalType wrapped_type;
	if (!ParseLogicalType(type_str, wrapped_type, error, 0)) {
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

} // namespace harbor_sql
} // namespace duckdb
