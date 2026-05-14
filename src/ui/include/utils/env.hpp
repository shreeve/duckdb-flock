#pragma once

#include <string>
#include <cstdint>

namespace duckdb {

const char *TryGetEnv(const char *name);

std::string GetEnvOrDefault(const char *name, const char *default_value);

uint32_t GetEnvOrDefaultInt(const char *name, uint32_t default_value);

bool IsEnvEnabled(const char *name);

} // namespace duckdb
