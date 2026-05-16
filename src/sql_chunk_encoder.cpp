// PR-5: SqlChunkEncoder — per-LogicalTypeId NDJSON encoding per SPEC §5.4.
//
// Each LogicalTypeId branch in EmitColumnSchema and EmitValue maps to
// a row in SPEC §5.4's "Row encoding rules" table. Keep the two
// switches in sync: any new schema-emitted shape must have a matching
// value-emit branch (and vice-versa), or clients won't be able to
// decode.
//
// "Lossless" guarantee: every core LogicalType listed in SPEC §5.4
// round-trips byte-equal when decoded with the schema record. Extension
// types not in the table fall back to CAST(... AS VARCHAR) and are
// flagged "lossless":false in the schema; clients can either accept
// the lossy text representation or refuse the column.

#include "sql_chunk_encoder.hpp"

#include "sql_json_writer.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <utility>

namespace duckdb {
namespace harbor_sql {

namespace {

// Decimal as fixed-precision string preserving width/scale.
std::string DecimalToString(const Value &v, uint8_t width, uint8_t scale) {
	switch (v.type().InternalType()) {
	case PhysicalType::INT16:
		return Decimal::ToString(v.GetValueUnsafe<int16_t>(), width, scale);
	case PhysicalType::INT32:
		return Decimal::ToString(v.GetValueUnsafe<int32_t>(), width, scale);
	case PhysicalType::INT64:
		return Decimal::ToString(v.GetValueUnsafe<int64_t>(), width, scale);
	case PhysicalType::INT128:
		return Decimal::ToString(v.GetValueUnsafe<hugeint_t>(), width, scale);
	default:
		return v.ToString();
	}
}

// Hugeint-to-decimal-string. Preserves full ±170141183460469231731687303715884105727 range.
std::string HugeintToString(hugeint_t v) {
	return Hugeint::ToString(v);
}

std::string UhugeintToString(uhugeint_t v) {
	return Uhugeint::ToString(v);
}

// Base64 encode arbitrary bytes; matches DuckDB's Blob::ToBase64 output.
std::string Base64Encode(const string_t &blob) {
	auto required = Blob::ToBase64Size(blob);
	std::string out;
	out.resize(required);
	Blob::ToBase64(blob, out.data());
	return out;
}

// Date as YYYY-MM-DD.
std::string DateToString(date_t d) {
	return Date::ToString(d);
}

// Time as HH:MM:SS[.ffffff].
std::string TimeToString(dtime_t t) {
	return Time::ToString(t);
}

// TimeTZ as HH:MM:SS[.ffffff]±HH:MM (RFC 3339 time form).
std::string TimeTzToString(dtime_tz_t t) {
	return Time::ToString(t.time()) + Time::ToUTCOffset(static_cast<int32_t>(t.offset() / Interval::SECS_PER_HOUR),
	                                                     (t.offset() % Interval::SECS_PER_HOUR) / 60);
}

// Timestamp without TZ: YYYY-MM-DDTHH:MM:SS[.ffffff], NO trailing Z.
// We use Timestamp::ToString which gives "YYYY-MM-DD HH:MM:SS[.ffffff]"
// and rewrite the space to 'T'.
std::string TimestampToIsoNaive(timestamp_t ts) {
	auto s = Timestamp::ToString(ts);
	auto pos = s.find(' ');
	if (pos != std::string::npos) {
		s[pos] = 'T';
	}
	return s;
}

// TimestampTZ in UTC: YYYY-MM-DDTHH:MM:SS[.ffffff]Z (matches RFC 3339).
std::string TimestampTzToIsoUtc(timestamp_t ts) {
	auto s = TimestampToIsoNaive(ts);
	s.push_back('Z');
	return s;
}

// SPEC §5.4: TIMESTAMP_S/MS/NS use the same date-time form but with
// fractional precision sized to the type (0 / 3 / 9 digits). DuckDB's
// internal storage normalizes to micros; converting back to the
// original precision requires knowing the source type.
std::string TimestampSecToIso(timestamp_t ts) {
	// PR-7e fix: DuckDB stores TIMESTAMP_S internally as int64 SECONDS
	// since epoch (the timestamp_t bit pattern IS that integer), but
	// Timestamp::ToString interprets its argument as MICROSECONDS since
	// epoch. Without conversion, a TIMESTAMP_S of 1778683496 seconds
	// (2026-05-16) renders as 1778683496 micros (1970-01-01 00:29:38).
	// Convert to canonical-micros via Timestamp::FromEpochSeconds.
	auto canonical = Timestamp::FromEpochSeconds(ts.value);
	auto s = TimestampToIsoNaive(canonical);
	// TIMESTAMP_S has zero fractional precision per SPEC §5.4;
	// strip any fraction the formatter may have added.
	auto dot = s.find('.');
	if (dot != std::string::npos) {
		s.resize(dot);
	}
	return s;
}

std::string TimestampMsToIso(timestamp_t ts) {
	// PR-7e fix: TIMESTAMP_MS is int64 MILLISECONDS since epoch in the
	// timestamp_t bit pattern. Convert via Timestamp::FromEpochMs to
	// canonical micros before calling the formatter.
	auto canonical = Timestamp::FromEpochMs(ts.value);
	auto s = TimestampToIsoNaive(canonical);
	auto dot = s.find('.');
	if (dot != std::string::npos) {
		// Keep up to 3 digits of fraction.
		auto end = std::min(s.size(), dot + 4);
		s.resize(end);
	}
	return s;
}

std::string TimestampNsToIso(const Value &v) {
	// DuckDB stores TIMESTAMP_NS as int64 nanoseconds since epoch in
	// timestamp_t. Use the type's own ToString to capture nanoseconds
	// fidelity; fall back to value-based ToString if specialized API
	// isn't reachable.
	auto s = v.ToString();
	auto pos = s.find(' ');
	if (pos != std::string::npos) {
		s[pos] = 'T';
	}
	return s;
}

// UUID as canonical lowercase 8-4-4-4-12.
std::string UuidToString(hugeint_t u) {
	return UUID::ToString(u);
}

// BIT as a string of '0'/'1'. DuckDB's Bit::ToString includes the
// leading-zero-pad byte; we want just the bit chars per SPEC §5.4.
std::string BitToString(const Value &v) {
	// Value::ToString on a BIT type returns the canonical hex/bit form;
	// for SPEC §5.4 we want the literal bit string. DuckDB's Value
	// representation already gives us "010110..." style; just pass
	// through.
	return v.ToString();
}

// SPEC §5.4 says JSON column values are emitted as JSON-text STRINGS
// (i.e. the on-the-wire form is `"{...}"` containing the JSON text,
// NOT raw nested JSON). This disambiguates SQL NULL from the JSON
// `null` literal: SQL NULL is JSON `null`; a JSON column containing
// the JSON value `null` is the STRING "null".
std::string JsonValueToString(const Value &v) {
	// JSON columns are stored internally as VARCHAR with the JSON
	// logical type alias; ToString gives us the canonical text.
	return v.ToString();
}

// Convert JSON Value of LIST<INTERVAL>'s INTERVAL value to {months,days,micros}.
// SPEC §5.4: micros as STRING (for full int64 precision through JS).
void EmitInterval(JsonWriter &out, interval_t iv) {
	out.BeginObject();
	out.KeyInt64("months", iv.months);
	out.KeyInt64("days", iv.days);
	// micros is int64 — emit as string for JS-safe round-trip
	char tmp[32];
	auto n = std::snprintf(tmp, sizeof(tmp), "%" PRId64, iv.micros);
	out.Key("micros");
	if (n > 0) {
		out.String(tmp, static_cast<std::size_t>(n));
	} else {
		out.String("0");
	}
	out.EndObject();
}

} // namespace

SqlChunkEncoder::SqlChunkEncoder(std::vector<std::string> column_names_p,
                                  std::vector<LogicalType> column_types_p)
    : column_names(std::move(column_names_p)), column_types(std::move(column_types_p)) {
	if (column_names.size() != column_types.size()) {
		throw InternalException("SqlChunkEncoder: names/types size mismatch (%zu vs %zu)",
		                        column_names.size(), column_types.size());
	}
}

SqlChunkEncoder::~SqlChunkEncoder() = default;

// ---------- schema emission ----------

void SqlChunkEncoder::EmitSchema(JsonWriter &out, const std::string &session_id) const {
	out.BeginObject();
	out.KeyString("type", "schema");
	if (!session_id.empty()) {
		out.KeyString("sessionId", session_id);
	}
	out.Key("columns");
	out.BeginArray();
	for (idx_t i = 0; i < column_names.size(); i++) {
		EmitColumnSchema(out, column_names[i], column_types[i]);
	}
	out.EndArray();
	out.EndObject();
}

void SqlChunkEncoder::EmitColumnSchema(JsonWriter &out, const std::string &name, const LogicalType &type) const {
	out.BeginObject();
	if (!name.empty()) {
		out.KeyString("name", name);
	}
	out.KeyString("duckdbType", type.ToString());

	// Emit per-type metadata. Lossless flag is true for everything
	// listed in SPEC §5.4; we set false ONLY in the extension-type
	// fallback below.
	switch (type.id()) {
	case LogicalTypeId::DECIMAL: {
		uint8_t width = DecimalType::GetWidth(type);
		uint8_t scale = DecimalType::GetScale(type);
		out.KeyBool("lossless", true);
		out.Key("decimal");
		out.BeginObject();
		out.KeyInt64("width", width);
		out.KeyInt64("scale", scale);
		out.EndObject();
		break;
	}
	case LogicalTypeId::LIST: {
		out.KeyBool("lossless", true);
		out.Key("child");
		EmitColumnSchema(out, "", ListType::GetChildType(type));
		break;
	}
	case LogicalTypeId::ARRAY: {
		out.KeyBool("lossless", true);
		out.KeyInt64("arrayLength", static_cast<int64_t>(ArrayType::GetSize(type)));
		out.Key("child");
		EmitColumnSchema(out, "", ArrayType::GetChildType(type));
		break;
	}
	case LogicalTypeId::STRUCT: {
		out.KeyBool("lossless", true);
		out.Key("fields");
		out.BeginArray();
		auto &child_types = StructType::GetChildTypes(type);
		for (auto &child : child_types) {
			EmitColumnSchema(out, child.first, child.second);
		}
		out.EndArray();
		break;
	}
	case LogicalTypeId::MAP: {
		out.KeyBool("lossless", true);
		out.Key("keyType");
		EmitColumnSchema(out, "", MapType::KeyType(type));
		out.Key("valueType");
		EmitColumnSchema(out, "", MapType::ValueType(type));
		out.KeyString("encoding", "pairs");
		break;
	}
	case LogicalTypeId::UNION: {
		out.KeyBool("lossless", true);
		out.Key("members");
		out.BeginArray();
		auto member_count = UnionType::GetMemberCount(type);
		for (idx_t i = 0; i < member_count; i++) {
			EmitColumnSchema(out, UnionType::GetMemberName(type, i), UnionType::GetMemberType(type, i));
		}
		out.EndArray();
		break;
	}
	case LogicalTypeId::ENUM: {
		out.KeyBool("lossless", true);
		out.Key("values");
		out.BeginArray();
		auto enum_size = EnumType::GetSize(type);
		auto &enum_vector = EnumType::GetValuesInsertOrder(type);
		auto enum_strings = FlatVector::GetData<string_t>(enum_vector);
		for (idx_t i = 0; i < enum_size; i++) {
			out.String(enum_strings[i].GetString());
		}
		out.EndArray();
		break;
	}
	default:
		// Core types listed in SPEC §5.4 are lossless. Anything we
		// don't recognize falls back to the CAST(... AS VARCHAR) path
		// in EmitValue and is flagged lossless:false here so clients
		// know to treat the column as text.
		switch (type.id()) {
		case LogicalTypeId::BOOLEAN:
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::HUGEINT:
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::UBIGINT:
		case LogicalTypeId::UHUGEINT:
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE:
		case LogicalTypeId::VARCHAR:
		case LogicalTypeId::UUID:
		case LogicalTypeId::DATE:
		case LogicalTypeId::TIME:
		case LogicalTypeId::TIME_TZ:
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_TZ:
		case LogicalTypeId::INTERVAL:
		case LogicalTypeId::BLOB:
		case LogicalTypeId::BIT:
		case LogicalTypeId::SQLNULL:
			out.KeyBool("lossless", true);
			break;
		default:
			// User-defined / extension types fall back to text.
			out.KeyBool("lossless", false);
			out.KeyString("encoding", "varchar-cast");
			break;
		}
		break;
	}
	out.EndObject();
}

// ---------- value emission ----------

void SqlChunkEncoder::EmitValue(JsonWriter &out, const Value &v, const LogicalType &type) const {
	if (v.IsNull()) {
		out.Null();
		return;
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		out.Bool(v.GetValue<bool>());
		return;
	case LogicalTypeId::TINYINT:
		out.Int64(v.GetValue<int8_t>());
		return;
	case LogicalTypeId::SMALLINT:
		out.Int64(v.GetValue<int16_t>());
		return;
	case LogicalTypeId::INTEGER:
		out.Int64(v.GetValue<int32_t>());
		return;
	case LogicalTypeId::UTINYINT:
		out.Uint64(v.GetValue<uint8_t>());
		return;
	case LogicalTypeId::USMALLINT:
		out.Uint64(v.GetValue<uint16_t>());
		return;
	case LogicalTypeId::UINTEGER:
		out.Uint64(v.GetValue<uint32_t>());
		return;
	// SPEC §5.4: 64+-bit integers as STRING for JS-safe round-trip.
	case LogicalTypeId::BIGINT: {
		char tmp[32];
		auto n = std::snprintf(tmp, sizeof(tmp), "%" PRId64, v.GetValue<int64_t>());
		out.String(tmp, n > 0 ? static_cast<std::size_t>(n) : 0);
		return;
	}
	case LogicalTypeId::UBIGINT: {
		char tmp[32];
		auto n = std::snprintf(tmp, sizeof(tmp), "%" PRIu64, v.GetValue<uint64_t>());
		out.String(tmp, n > 0 ? static_cast<std::size_t>(n) : 0);
		return;
	}
	case LogicalTypeId::HUGEINT:
		out.String(HugeintToString(v.GetValueUnsafe<hugeint_t>()));
		return;
	case LogicalTypeId::UHUGEINT:
		out.String(UhugeintToString(v.GetValueUnsafe<uhugeint_t>()));
		return;
	case LogicalTypeId::FLOAT:
		out.Double(v.GetValue<float>());
		return;
	case LogicalTypeId::DOUBLE:
		out.Double(v.GetValue<double>());
		return;
	case LogicalTypeId::DECIMAL: {
		uint8_t width = DecimalType::GetWidth(type);
		uint8_t scale = DecimalType::GetScale(type);
		out.String(DecimalToString(v, width, scale));
		return;
	}
	case LogicalTypeId::VARCHAR:
		out.String(v.GetValue<std::string>());
		return;
	case LogicalTypeId::UUID:
		out.String(UuidToString(v.GetValueUnsafe<hugeint_t>()));
		return;
	case LogicalTypeId::DATE:
		out.String(DateToString(v.GetValueUnsafe<date_t>()));
		return;
	case LogicalTypeId::TIME:
		out.String(TimeToString(v.GetValueUnsafe<dtime_t>()));
		return;
	case LogicalTypeId::TIME_TZ:
		out.String(TimeTzToString(v.GetValueUnsafe<dtime_tz_t>()));
		return;
	case LogicalTypeId::TIMESTAMP:
		out.String(TimestampToIsoNaive(v.GetValueUnsafe<timestamp_t>()));
		return;
	case LogicalTypeId::TIMESTAMP_SEC:
		out.String(TimestampSecToIso(v.GetValueUnsafe<timestamp_t>()));
		return;
	case LogicalTypeId::TIMESTAMP_MS:
		out.String(TimestampMsToIso(v.GetValueUnsafe<timestamp_t>()));
		return;
	case LogicalTypeId::TIMESTAMP_NS:
		out.String(TimestampNsToIso(v));
		return;
	case LogicalTypeId::TIMESTAMP_TZ:
		out.String(TimestampTzToIsoUtc(v.GetValueUnsafe<timestamp_t>()));
		return;
	case LogicalTypeId::INTERVAL:
		EmitInterval(out, v.GetValueUnsafe<interval_t>());
		return;
	case LogicalTypeId::BLOB: {
		auto str = v.GetValueUnsafe<string_t>();
		out.String(Base64Encode(str));
		return;
	}
	case LogicalTypeId::BIT:
		out.String(BitToString(v));
		return;
	case LogicalTypeId::LIST: {
		auto &list_values = ListValue::GetChildren(v);
		auto &child_type = ListType::GetChildType(type);
		out.BeginArray();
		for (auto &elem : list_values) {
			EmitValue(out, elem, child_type);
		}
		out.EndArray();
		return;
	}
	case LogicalTypeId::ARRAY: {
		auto &arr_values = ArrayValue::GetChildren(v);
		auto &child_type = ArrayType::GetChildType(type);
		out.BeginArray();
		for (auto &elem : arr_values) {
			EmitValue(out, elem, child_type);
		}
		out.EndArray();
		return;
	}
	case LogicalTypeId::STRUCT: {
		auto &struct_values = StructValue::GetChildren(v);
		auto &child_types = StructType::GetChildTypes(type);
		// Bounds-safe iteration in case of unexpected mismatch.
		auto bound = std::min(struct_values.size(), child_types.size());
		out.BeginObject();
		for (idx_t i = 0; i < bound; i++) {
			out.Key(child_types[i].first);
			EmitValue(out, struct_values[i], child_types[i].second);
		}
		out.EndObject();
		return;
	}
	case LogicalTypeId::MAP: {
		// MAP is stored as LIST<STRUCT<key, value>>; SPEC §5.4 emits
		// it as an ARRAY of [K, V] pairs.
		auto &map_entries = ListValue::GetChildren(v);
		auto &key_type = MapType::KeyType(type);
		auto &value_type = MapType::ValueType(type);
		out.BeginArray();
		for (auto &entry : map_entries) {
			auto &kv = StructValue::GetChildren(entry); // [key, value]
			out.BeginArray();
			EmitValue(out, kv[0], key_type);
			EmitValue(out, kv[1], value_type);
			out.EndArray();
		}
		out.EndArray();
		return;
	}
	case LogicalTypeId::UNION: {
		// SPEC §5.4: UNION as {"tag":"member_name","value":...}.
		// DuckDB stores UNION as STRUCT<tag, member0, member1, ...>;
		// the active member is identified by the tag.
		auto tag = UnionValue::GetTag(v);
		auto member_name = UnionType::GetMemberName(type, tag);
		auto &member_type = UnionType::GetMemberType(type, tag);
		auto member_value = UnionValue::GetValue(v);
		out.BeginObject();
		out.KeyString("tag", member_name);
		out.Key("value");
		EmitValue(out, member_value, member_type);
		out.EndObject();
		return;
	}
	case LogicalTypeId::ENUM:
		// Enum stored as the underlying integer; the catalog has the
		// label table. Value::ToString returns the label.
		out.String(v.ToString());
		return;
	case LogicalTypeId::SQLNULL:
		out.Null();
		return;
	default: {
		// Extension / user-defined type — fall back to CAST AS VARCHAR.
		// The schema flagged this as lossless:false; clients know the
		// column is text-only.
		// Special-case JSON-aliased VARCHAR (the json extension's JSON
		// type is a VARCHAR alias internally; if we got here it means
		// it wasn't recognized as VARCHAR, so emit ToString and trust
		// it).
		out.String(v.ToString());
		return;
	}
	}
}

// ---------- row / chunk emission ----------

void SqlChunkEncoder::EmitValuesArray(JsonWriter &out, const DataChunk &chunk, idx_t row_idx) const {
	out.BeginArray();
	for (idx_t col = 0; col < column_types.size(); col++) {
		auto v = chunk.GetValue(col, row_idx);
		EmitValue(out, v, column_types[col]);
	}
	out.EndArray();
}

void SqlChunkEncoder::EmitRow(JsonWriter &out, const DataChunk &chunk, idx_t row_idx) const {
	out.BeginObject();
	out.KeyString("type", "row");
	out.Key("values");
	EmitValuesArray(out, chunk, row_idx);
	out.EndObject();
}

void SqlChunkEncoder::EmitChunk(JsonWriter &out, const DataChunk &chunk) const {
	out.BeginObject();
	out.KeyString("type", "chunk");
	out.Key("rows");
	out.BeginArray();
	for (idx_t row = 0; row < chunk.size(); row++) {
		EmitValuesArray(out, chunk, row);
	}
	out.EndArray();
	out.EndObject();
}

void SqlChunkEncoder::EmitEnd(JsonWriter &out, idx_t row_count, std::int64_t time_ms, bool truncated) const {
	out.BeginObject();
	out.KeyString("type", "end");
	out.KeyUint64("rowCount", row_count);
	out.KeyInt64("timeMs", time_ms);
	if (truncated) {
		out.KeyBool("truncated", true);
	}
	out.EndObject();
}

void SqlChunkEncoder::EmitError(JsonWriter &out, const std::string &error_code,
                                 const std::string &message) const {
	out.BeginObject();
	out.KeyString("type", "error");
	out.KeyString("code", error_code);
	out.KeyString("message", message);
	out.EndObject();
}

// ---------- one-shot ----------

void SqlChunkEncoder::EmitOneShot(JsonWriter &out, const std::string &session_id, const std::string &kind,
                                   const std::vector<reference<DataChunk>> &chunks, idx_t row_count,
                                   std::int64_t time_ms) const {
	out.BeginObject();
	out.KeyBool("ok", true);
	out.KeyString("kind", kind);
	if (!session_id.empty()) {
		out.KeyString("sessionId", session_id);
	}
	out.Key("columns");
	out.BeginArray();
	for (idx_t i = 0; i < column_names.size(); i++) {
		EmitColumnSchema(out, column_names[i], column_types[i]);
	}
	out.EndArray();
	out.Key("data");
	out.BeginArray();
	for (auto &chunk_ref : chunks) {
		auto &chunk = chunk_ref.get();
		for (idx_t row = 0; row < chunk.size(); row++) {
			EmitValuesArray(out, chunk, row);
		}
	}
	out.EndArray();
	out.KeyUint64("rowCount", row_count);
	out.KeyInt64("timeMs", time_ms);
	out.EndObject();
}

void SqlChunkEncoder::EmitOneShotWrite(JsonWriter &out, const std::string &session_id, idx_t affected,
                                        std::int64_t time_ms) const {
	out.BeginObject();
	out.KeyBool("ok", true);
	out.KeyString("kind", "write");
	if (!session_id.empty()) {
		out.KeyString("sessionId", session_id);
	}
	out.KeyUint64("rowCount", affected);
	out.KeyInt64("timeMs", time_ms);
	out.EndObject();
}

} // namespace harbor_sql
} // namespace duckdb
