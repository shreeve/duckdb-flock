#pragma once

// Lifecycle SQL functions per SPEC §9:
//
//   harbor_serve(bind := '127.0.0.1', port := 9494, token := NULL)
//     → row of (listen_uri, listen_url, auth_token)
//   harbor_stop()      → row of (status)   -- stops THE running server
//                                            (single-server-per-process)
//   harbor_stop(uri)   → row of (status)   -- stops the server matching
//                                            the URI; preserved as the
//                                            originally-shipped form
//                                            and for symmetry with
//                                            quack_stop(uri)
//   harbor_wait()      → row of (BOOLEAN ok) -- blocks until clean
//                                              shutdown or signal;
//                                              throws if no server
//
// All are TABLE FUNCTIONS (not scalars) because they return rows with
// named columns. Single-server-per-process: harbor_serve throws if a
// server is already running.
//
// quack_serve / quack_stop are kept as thin shims in quack_start_stop.cpp;
// they delegate to HarborServerState::Global() under the hood. (There
// is no quack_stop() no-arg overload — upstream quack always required
// the URI; we preserve that for backwards-compatibility on the
// quack_*-named surface.)
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
	// Returns a TableFunctionSet containing both overloads:
	//   harbor_stop()
	//   harbor_stop(VARCHAR)
	static TableFunctionSet GetFunction();
};

class HarborWaitFunction {
public:
	static TableFunction GetFunction();
};

} // namespace duckdb
