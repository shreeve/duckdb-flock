#pragma once

// PR-2 refactor of upstream duckdb-quack's quack_storage.hpp.
//
// Upstream's QuackStorageExtensionInfo carried the multi-server map
// (`unordered_map<string, unique_ptr<QuackServer>>`) plus the
// CreateServer / StopServer / ListServers methods. flock is
// single-server-per-process per SPEC §2 — that state lives in
// FlockServerState::Global() (in src/include/flock_http_server.hpp).
//
// What stays here:
//   - QuackStorageExtension: the StorageExtension subclass that wires
//     up the ATTACH 'quack:host' callback and transaction manager.
//     Stock-quack clients reach this via StorageExtension::Find("quack").
//   - QuackStorageExtensionInfo: kept as an empty StorageExtensionInfo
//     subclass so the existing registration pattern in quack_extension.cpp
//     stays mechanically identical. No data members; no server map.
//     The STORAGE_EXTENSION_KEY constant stays "quack" for ATTACH compat.

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

class QuackStorageExtension : public StorageExtension {
public:
	QuackStorageExtension();
};

class QuackStorageExtensionInfo : public StorageExtensionInfo {
public:
	static constexpr const char *STORAGE_EXTENSION_KEY = "quack";
	// PR-2: the upstream multi-server CreateServer / StopServer / ListServers
	// API is gone. Use FlockServerState::Global().Start() / .Stop() /
	// .WithCurrent() / .IsRunning() instead. See SPEC §2 single-server-
	// per-process.
};

} // namespace duckdb
