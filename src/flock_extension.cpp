// FlockExtension implementation. PR-1 delegates to the vendored
// QuackExtension. The flock-specific surface (flock_version() scalar,
// extension Name() == "flock", DUCKDB_CPP_EXTENSION_ENTRY(flock,...))
// is registered inside QuackExtension::Load — see the header comment in
// src/quack/quack_extension.cpp for the four flock-specific edits to
// that file.
//
// PR-2's architectural refactor may collapse this delegation by moving
// the LoadInternal body here and dropping QuackExtension entirely.

#include "flock_extension.hpp"
#include "quack_extension.hpp"

namespace duckdb {

void FlockExtension::Load(ExtensionLoader &loader) {
	QuackExtension delegate;
	delegate.Load(loader);
}

std::string FlockExtension::Name() {
	return "flock";
}

std::string FlockExtension::Version() const {
	return QuackExtension().Version();
}

} // namespace duckdb
