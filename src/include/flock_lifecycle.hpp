#pragma once

// Lifecycle SQL functions per SPEC §9:
//
//   flock_serve(uri, token := NULL, allow_other_hostname := false)
//     → row of (listen_uri, listen_url, auth_token)
//   flock_stop(uri) → row of (status)  -- BOOLEAN-shaped per SPEC; we
//                                         actually return a status string
//                                         to match upstream quack_stop
//                                         output for compatibility
//   flock_wait() → row of (BOOLEAN ok) -- blocks until clean shutdown
//                                         or signal; throws if no server
//
// All three are TABLE FUNCTIONS (not scalars) because they return rows
// with named columns. Single-server-per-process: flock_serve throws if
// a server is already running.
//
// quack_serve / quack_stop are kept as thin shims in quack_start_stop.cpp;
// they delegate to FlockServerState::Global() under the hood.
//
// quack_wait does not exist upstream — flock_wait is new in flock; it
// arrived in PR-2 specifically to support the `duckdb -no-stdin -init …`
// daemon-mode pattern (per SPEC §2 "Daemon mode"). There is no
// `quack_wait` alias.

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class FlockServeFunction {
public:
	static TableFunctionSet GetFunction();
};

class FlockStopFunction {
public:
	static TableFunction GetFunction();
};

class FlockWaitFunction {
public:
	static TableFunction GetFunction();
};

} // namespace duckdb
