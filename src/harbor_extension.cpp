// HarborExtension implementation. PR-1 delegates to the vendored
// QuackExtension. The harbor-specific surface (harbor_version() scalar,
// extension Name() == "harbor", DUCKDB_CPP_EXTENSION_ENTRY(harbor,...))
// is registered inside QuackExtension::Load — see the header comment in
// src/quack/quack_extension.cpp for the four harbor-specific edits to
// that file.
//
// PR-2's architectural refactor may collapse this delegation by moving
// the LoadInternal body here and dropping QuackExtension entirely.

#include "harbor_extension.hpp"
#include "quack_extension.hpp"

namespace duckdb {

void HarborExtension::Load(ExtensionLoader &loader) {
	QuackExtension delegate;
	delegate.Load(loader);
}

std::string HarborExtension::Name() {
	return "harbor";
}

std::string HarborExtension::Version() const {
	return QuackExtension().Version();
}

} // namespace duckdb
