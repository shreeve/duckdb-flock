#pragma once

// Static-link façade for the flock extension.
//
// DuckDB's build system codegens, for any statically linked extension named
// `<name>`:
//
//   #include "<name>_extension.hpp"
//   db.LoadStaticExtension<<Name>Extension>();
//
// (See duckdb/extension/CMakeLists.txt's EXT_NAME_CAMELCASE loop.)
//
// For our extension named `flock`, that means DuckDB looks for a header
// `flock_extension.hpp` exposing `class FlockExtension : public Extension`.
//
// PR-1 keeps the actual implementation in `class QuackExtension`
// (declared in `quack_extension.hpp`, defined in `quack_extension.cpp`)
// to minimize the diff against upstream duckdb-quack. This file is a
// thin subclass that satisfies the static-link contract without
// renaming the implementation. PR-2 may rename the C++ class to
// FlockExtension proper and remove this façade.
//
// File location: `src/include/flock_extension.hpp` is the default
// INCLUDE_DIR DuckDB uses for an extension whose `duckdb_extension_load`
// call doesn't set INCLUDE_DIR explicitly (see
// duckdb/extension/extension_build_tools.cmake's `duckdb_extension_load`,
// `INCLUDE_PATH_DEFAULT`). Don't move this file without also setting
// INCLUDE_DIR in `extension_config.cmake`.
//
// Method dispatch: Extension::Load() and Extension::Name() are virtual
// in the DuckDB base, and QuackExtension overrides both. Inheriting
// from QuackExtension and providing no body means LoadStaticExtension's
// `T extension; extension.Load(loader); extension.Name();` calls
// dispatch to QuackExtension's implementations — which already report
// "flock" as the name and register the full LoadInternal surface
// (including the flock_version() scalar).

#include "quack_extension.hpp"

namespace duckdb {

class FlockExtension : public QuackExtension {};

} // namespace duckdb
