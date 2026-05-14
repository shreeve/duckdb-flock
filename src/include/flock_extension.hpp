#pragma once

// Static-link façade for the flock extension.
//
// DuckDB's build system codegens, for any statically linked extension named
// `<name>`, an include of `<name>_extension.hpp` and a call to
// `db.LoadStaticExtension<<Name>Extension>()`. For our extension named
// `flock`, that means DuckDB looks for a header `flock_extension.hpp`
// exposing `class FlockExtension : public Extension`.
//
// File location: `src/include/flock_extension.hpp` is the default
// INCLUDE_DIR DuckDB uses for an extension whose `duckdb_extension_load`
// call doesn't set INCLUDE_DIR explicitly (see
// duckdb/extension/extension_build_tools.cmake's `duckdb_extension_load`,
// `INCLUDE_PATH_DEFAULT`). Don't move this file without also setting
// INCLUDE_DIR in `extension_config.cmake`.
//
// Self-contained on purpose: when DuckDB's static-link codegen processes
// this header, only the extension's INCLUDE_DIR is on the search path —
// not src/quack/include where quack_extension.hpp lives. So this header
// declares FlockExtension standalone (does NOT inherit from
// QuackExtension) and the implementation in src/flock_extension.cpp
// delegates internally. The implementation .cpp is compiled with our
// flock CMakeLists' include paths (which DO see src/quack/include) and
// can include quack_extension.hpp normally.
//
// PR-2 may collapse FlockExtension and QuackExtension into one class
// once the architectural refactor is done; for PR-1 the delegation is
// the minimum-diff way to satisfy DuckDB's static-link contract.

#include "duckdb/main/extension.hpp"

namespace duckdb {

class FlockExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
