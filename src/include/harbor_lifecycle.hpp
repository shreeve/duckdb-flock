#pragma once

// Lifecycle SQL functions per SPEC §9:
//
//   harbor_serve(uri, token := NULL, allow_other_hostname := false)
//     → row of (listen_uri, listen_url, auth_token)
//   harbor_stop(uri) → row of (status)  -- BOOLEAN-shaped per SPEC; we
//                                         actually return a status string
//                                         to match upstream quack_stop
//                                         output for compatibility
//   harbor_wait() → row of (BOOLEAN ok) -- blocks until clean shutdown
//                                         or signal; throws if no server
//
// All three are TABLE FUNCTIONS (not scalars) because they return rows
// with named columns. Single-server-per-process: harbor_serve throws if
// a server is already running.
//
// quack_serve / quack_stop are kept as thin shims in quack_start_stop.cpp;
// they delegate to HarborServerState::Global() under the hood.
//
// quack_wait does not exist upstream — harbor_wait is new in harbor; it
// arrived in PR-2 specifically to support the `duckdb -no-stdin -init …`
// daemon-mode pattern (per SPEC §2 "Daemon mode"). There is no
// `quack_wait` alias.

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class HarborServeFunction {
public:
	static TableFunctionSet GetFunction();
};

class HarborStopFunction {
public:
	static TableFunction GetFunction();
};

class HarborWaitFunction {
public:
	static TableFunction GetFunction();
};

} // namespace duckdb
