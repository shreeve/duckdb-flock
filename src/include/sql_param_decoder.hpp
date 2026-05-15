#pragma once

// SqlParamDecoder — JSON params → DuckDB Value, per SPEC §5.2.
//
// Two modes (mixable within a single params array):
//
//   Mode A — implicit: bare JSON value coerced to the prepared
//            statement's expected parameter type (server-side
//            introspection of PreparedStatement::expected_parameter_types).
//
//   Mode B — typed wrapper: {"type":"DECIMAL(38,4)","value":"..."}
//            with an explicit DuckDB-LogicalType-string `type` field.
//            Used when the prepared statement has no expected type
//            to coerce against (bare SELECT $1) or when the caller
//            wants to be unambiguous.
//
// Errors return BAD_REQUEST with a descriptive message; the SqlHandlers
// caller wraps that in the standard error envelope. The decoder never
// throws InternalException — every failure is a caller-input bug.
//
// Hand-rolled JSON parser: kept tiny on purpose. The /sql endpoint
// only needs to parse the request body's `params` array (and Mode B's
// nested `value` field). We never see arbitrary deep nesting in the
// wire format. yyjson would be overkill and would couple our parser
// version to httpfs's.

#include <cstdint>
#include <string>
#include <vector>

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class ClientContext;
class LogicalType;

namespace flock_sql {

struct DecodedParams {
	// duckdb::vector (a public subclass of std::vector) is required so
	// the result can be passed by reference to PreparedStatement::Execute
	// — `Execute(vector<Value>&, bool)` won't bind to std::vector& and
	// the variadic Execute<ARGS...> overload would be picked instead,
	// failing Value::CreateValue<vector<Value>> at static_assert time.
	vector<Value> values;
	// On parse/coerce failure, ok=false and error has a human message.
	bool ok = true;
	std::string error;
};

class SqlParamDecoder {
public:
	SqlParamDecoder() = default;

	// Decode the `params` JSON array (raw text — caller has already
	// extracted it from the request body) against the prepared
	// statement's expected types. `params_json` MUST be a JSON array
	// (`[...]`); empty array OK. `expected_types` size must match
	// the number of parameters in `params_json` (caller validates
	// arity beforehand).
	//
	// `context` is needed only for Mode B's typed-wrapper parsing
	// (`TransformStringToLogicalType` requires a ClientContext to
	// resolve user-defined / extension types). Mode A scalar
	// coercion does not touch the context.
	//
	// Returns DecodedParams with ok=true on success; ok=false on any
	// parse/coerce error.
	DecodedParams Decode(const std::string &params_json, const vector<LogicalType> &expected_types,
	                     ClientContext &context);

	// Convenience: parse the FULL request body and extract `sql`,
	// `params`, `sessionId`. Hand-rolled minimal JSON object parser
	// for these three keys; rejects schemas more complex than what
	// SPEC §5.2 documents.
	struct ParsedRequest {
		std::string sql;
		std::string params_json; // raw text for `params` value, "" if absent
		std::string session_id;
		bool ok = true;
		std::string error;
	};
	static ParsedRequest ParseRequest(const std::string &body);

private:
	// Decode a single JSON value, possibly Mode B wrapper, into a
	// DuckDB Value coerced to `expected_type` (Mode A) or to the
	// wrapper's declared type (Mode B). `pos` is the start byte;
	// updated to point one past the parsed value.
	bool DecodeOne(const std::string &json, std::size_t &pos, const LogicalType &expected_type, Value &out,
	               ClientContext &context, std::string &error);

	// Mode B handling: parse `{"type":"X","value":...}`. Returns a
	// Value of the wrapper's declared type.
	bool DecodeTypedWrapper(const std::string &json, std::size_t &pos, Value &out, ClientContext &context,
	                         std::string &error);

	// JSON parsing primitives (whitespace-aware; advance `pos`).
	void SkipWhitespace(const std::string &json, std::size_t &pos);
	bool ConsumeChar(const std::string &json, std::size_t &pos, char expected, std::string &error);
	bool ParseString(const std::string &json, std::size_t &pos, std::string &out, std::string &error);
	bool ParseRawValue(const std::string &json, std::size_t &pos, std::string &raw_out, std::string &error);
};

} // namespace flock_sql
} // namespace duckdb
