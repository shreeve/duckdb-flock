#pragma once

// JsonWriter — minimal output-only JSON helper, tuned for /sql NDJSON.
//
// Why not yyjson: yyjson is primarily a parser; its writer API exists
// but pulls in extra surface for capabilities we don't need (we never
// emit user-controlled keys for objects; we emit a fixed set of
// shapes). Roll-our-own is ~200 LOC and covers exactly what SPEC §5.4
// needs: numbers (integer + double, NaN/Inf as strings), strings
// (with full RFC 8259 escape coverage including control chars and
// invalid UTF-8 replacement), arrays, objects, and a "raw" emitter
// for pre-encoded JSON-text values (e.g. the JSON column type).
//
// The writer appends to an internal std::string buffer; the caller
// pulls Take()/Str() at the end and writes to the network sink in
// one shot. This is the "buffer-before-write" discipline GPT-5.5
// flagged in round 15: never half-write a JSON value to the network
// and then throw — that would corrupt the NDJSON stream.
//
// Design notes:
//   - Numbers use fixed C-locale formatting (snprintf with C locale)
//     so a non-C system locale doesn't accidentally emit "1,5" for a
//     DECIMAL.
//   - String escape covers RFC 8259 §7: " \ control chars (\u00XX
//     for U+0000..U+001F), preserves valid UTF-8 byte sequences,
//     replaces invalid UTF-8 bytes with U+FFFD (\uFFFD).
//   - NO pretty-printing. NDJSON is one object per line; whitespace
//     between tokens is forbidden.
//   - The writer is NOT thread-safe; each request handler builds its
//     own writer per provider call.

#include <cstddef>
#include <cstdint>
#include <string>

namespace duckdb {
namespace flock_sql {

class JsonWriter {
public:
	JsonWriter() = default;

	// Container management. Emits commas between elements automatically;
	// the caller never writes a comma directly.
	void BeginObject();
	void EndObject();
	void BeginArray();
	void EndArray();

	// Object key. Must be called between BeginObject and EndObject; emits
	// the JSON-escaped key followed by ':'. Key is assumed to be ASCII
	// (we control all object keys in SPEC §5.4 — no user input here),
	// so no UTF-8 validation; just RFC 8259 string escape.
	void Key(const char *key);
	void Key(const std::string &key);

	// Value emitters. Each one knows whether to prefix a comma based on
	// the surrounding container state (managed by a stack of frames
	// inside the writer).
	void Null();
	void Bool(bool v);
	void Int64(int64_t v);
	void Uint64(uint64_t v);
	void Double(double v);
	// Emit a JSON string. Performs full escape including control chars
	// and invalid-UTF-8 replacement.
	void String(const char *s, std::size_t len);
	void String(const std::string &s);
	// Emit a number-shaped JSON value as-is (no quoting). Used when the
	// caller has already produced a canonical numeric or NaN/Inf string
	// representation. Callers must guarantee `s` is valid JSON syntax.
	void RawNumber(const std::string &s);
	// Emit a JSON-text value as-is (no quoting, no escaping). Used for
	// the JSON column type (per SPEC §5.4: emit as a JSON-text STRING,
	// so the on-the-wire value is `"..."` containing the JSON text).
	// Wait: the SPEC §5.4 row encoding for JSON columns is "string
	// containing canonical JSON text" — i.e. quoted-and-escaped, NOT
	// raw nested. So this Raw() is reserved for a hypothetical future
	// case (e.g. emitting pre-validated JSON literals from internal
	// state); /sql callers should use String() for JSON-typed values.
	void Raw(const std::string &raw_json_token);

	// Convenience: emit "key": <value> in one call.
	void KeyNull(const char *key);
	void KeyBool(const char *key, bool v);
	void KeyInt64(const char *key, int64_t v);
	void KeyUint64(const char *key, uint64_t v);
	void KeyDouble(const char *key, double v);
	void KeyString(const char *key, const std::string &v);
	void KeyRawNumber(const char *key, const std::string &v);

	// Final output. Take() empties the internal buffer; Str() returns
	// a const reference for inspection.
	std::string Take();
	const std::string &Str() const {
		return buf;
	}
	std::size_t Size() const {
		return buf.size();
	}
	bool Empty() const {
		return buf.empty();
	}

	// Reset the writer to initial state without freeing the buffer
	// (lets a streaming handler reuse it across DataChunks).
	void Reset();

private:
	enum class Frame : std::uint8_t {
		// Top-level — the next value is the first; no separator needed.
		ROOT,
		// Inside an object expecting a key (or end). Comma separator
		// before the next key after the first.
		OBJECT_KEY,
		// Inside an object expecting a value (just emitted a key + ':').
		OBJECT_VALUE,
		// Inside an array expecting a value (or end). Comma separator
		// before the next value after the first.
		ARRAY,
	};

	// Called by every value emitter. Writes a leading comma if the
	// current frame is mid-array or mid-object-after-first-element.
	// Updates the frame state to reflect that one more value just
	// landed (transitions OBJECT_VALUE → OBJECT_KEY etc.).
	void BeforeValue();

	// Called by Key(). Writes a leading comma if not the first key,
	// then transitions to OBJECT_VALUE.
	void BeforeKey();

	// Inner string-escape implementation. Wraps `len` bytes of `s`
	// with quotes, escapes per RFC 8259, replaces invalid UTF-8 bytes
	// with U+FFFD.
	void WriteEscapedString(const char *s, std::size_t len);

	std::string buf;
	// Stack of containers. Bottom is always ROOT. Each BeginObject /
	// BeginArray pushes; End* pops. Inside a container we alternate
	// between OBJECT_KEY <-> OBJECT_VALUE on Key() / value emitters,
	// or stay in ARRAY on each value.
	std::string frames; // each char is a Frame value (saves a vector header)
	// True iff the frame just opened and no value has been emitted yet
	// (used to suppress the leading comma for the first element).
	bool first_in_frame = true;
};

} // namespace flock_sql
} // namespace duckdb
