#pragma once

// SqlChunkEncoder — DuckDB DataChunk → NDJSON per SPEC §5.4.
//
// Responsibilities:
//   - Emit the {"type":"schema",...} record with full LogicalType
//     metadata (decimal width/scale, list/struct/map child types,
//     union members, enum values) so a client can decode rows
//     losslessly.
//   - Emit per-row values per the SPEC §5.4 type table:
//       * BIGINT/HUGEINT/UBIGINT/UHUGEINT as JSON numbers inside the
//         JavaScript safe-integer range, JSON strings outside it
//       * DECIMAL as STRING preserving width/scale
//       * UUID as canonical lowercase
//       * DATE/TIME/TIMESTAMP/TIMESTAMPTZ as ISO 8601 forms
//       * INTERVAL as {months,days,micros} object (micros as string)
//       * BLOB as base64
//       * BIT as string of '0'/'1'
//       * JSON column as JSON-text STRING (disambiguates SQL NULL vs JSON null)
//       * LIST/ARRAY recursively
//       * STRUCT as object keyed by field name
//       * MAP as ARRAY of [K,V] pairs (NOT object — keys may be non-string)
//       * ENUM as label string
//       * UNION as {"tag":"member","value":...}
//       * Extension types fall back to CAST AS VARCHAR with lossless:false on schema
//   - Emit {"type":"end",...} with rowCount/timeMs/optional truncated.
//   - Emit {"type":"error",...} for mid-stream failures.
//   - Emit one-shot buffered JSON for Accept: application/json.
//
// Buffer-before-write discipline: every Emit* method appends to a
// caller-provided JsonWriter. The handler reads JsonWriter::Take()
// after each Emit* call and writes the resulting string to the
// network sink in one shot. Never half-write a row to the network
// (round-15 GPT-5.5 catch).

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

class DataChunk;
class LogicalType;

namespace harbor_sql {

class JsonWriter;

class SqlChunkEncoder {
public:
	SqlChunkEncoder(std::vector<std::string> column_names, std::vector<LogicalType> column_types);
	~SqlChunkEncoder();

	SqlChunkEncoder(const SqlChunkEncoder &) = delete;
	SqlChunkEncoder &operator=(const SqlChunkEncoder &) = delete;

	// Emit the {"type":"schema",...} NDJSON line into `out` (without
	// trailing newline; caller appends '\n' before sink.write).
	void EmitSchema(JsonWriter &out, const std::string &session_id) const;

	// Emit one row of `chunk` as {"type":"row","values":[...]} into `out`.
	void EmitRow(JsonWriter &out, const DataChunk &chunk, idx_t row_idx) const;

	// Emit ALL rows of `chunk` as {"type":"chunk","rows":[[...],[...]]} into `out`.
	// (Chunk-bundled mode for Accept: application/x-ndjson; shape=chunk.)
	void EmitChunk(JsonWriter &out, const DataChunk &chunk) const;

	// Emit the final {"type":"end","rowCount":N,"timeMs":T} (with optional
	// "truncated":true field). One line.
	void EmitEnd(JsonWriter &out, idx_t row_count, std::int64_t time_ms, bool truncated) const;

	// Emit a mid-stream {"type":"error","code":"...","message":"..."} line.
	// HTTP status stays 200 because headers are already sent (SPEC §5.2).
	void EmitError(JsonWriter &out, const std::string &error_code, const std::string &message) const;

	// One-shot mode (Accept: application/json): emit the full response body
	// {"ok":true,"kind":"select","sessionId":"...","columns":[...],"data":[[...]],"rowCount":N,"timeMs":T}.
	// `kind` is "select" or "write".
	void EmitOneShot(JsonWriter &out, const std::string &session_id, const std::string &kind,
	                 const std::vector<reference<DataChunk>> &chunks, idx_t row_count,
	                 std::int64_t time_ms) const;

	// One-shot for write statements (DDL/DML without RETURNING). No
	// columns/data; just {"ok":true,"kind":"write","sessionId":"...","rowCount":N,"timeMs":T}.
	// rowCount is the affected-rows count if available, else 0.
	void EmitOneShotWrite(JsonWriter &out, const std::string &session_id, idx_t affected,
	                      std::int64_t time_ms) const;

	const std::vector<std::string> &Names() const {
		return column_names;
	}
	const std::vector<LogicalType> &Types() const {
		return column_types;
	}

private:
	// Emit the schema entry for one column. Recursive for nested types.
	// `name` is empty when emitting child types of LIST/ARRAY/STRUCT.
	void EmitColumnSchema(JsonWriter &out, const std::string &name, const LogicalType &type) const;

	// Emit one Value into `out`, applying the SPEC §5.4 row encoding rules.
	// Recursive for nested types (LIST, ARRAY, STRUCT, MAP, UNION).
	void EmitValue(JsonWriter &out, const Value &v, const LogicalType &type) const;

	// Emit just the [..., ..., ...] values array for one row of `chunk`.
	void EmitValuesArray(JsonWriter &out, const DataChunk &chunk, idx_t row_idx) const;

	std::vector<std::string> column_names;
	std::vector<LogicalType> column_types;
};

} // namespace harbor_sql
} // namespace duckdb
