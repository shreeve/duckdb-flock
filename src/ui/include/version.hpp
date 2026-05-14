#pragma once

// PR-3 transitional fork — see docs/upstream-ui-patches.md.
//
// Upstream duckdb-ui requires UI_EXTENSION_SEQ_NUM and UI_EXTENSION_GIT_SHA
// to be defined at build time (their CMakeLists.txt computes them from
// `git rev-list --count HEAD` and `git rev-parse --short=10 HEAD`).
//
// flock doesn't have a separate UI extension build cycle — UI is embedded
// in flock.duckdb_extension. We provide reasonable fallbacks so the
// vendored UI source compiles without requiring CMake-time git
// introspection. The version string is reported in /info's
// X-DuckDB-UI-Extension-Version header.
//
// To override: pass `-DUI_EXTENSION_SEQ_NUM="N" -DUI_EXTENSION_GIT_SHA="hash"`
// via EXT_FLAGS in the Makefile.

#ifndef UI_EXTENSION_SEQ_NUM
#define UI_EXTENSION_SEQ_NUM "0"
#endif
#ifndef UI_EXTENSION_GIT_SHA
#define UI_EXTENSION_GIT_SHA "embedded-in-flock"
#endif

#define UI_EXTENSION_VERSION UI_EXTENSION_SEQ_NUM "-" UI_EXTENSION_GIT_SHA
