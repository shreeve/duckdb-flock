#include "utils/env.hpp"

#include <cstdlib>
#include <duckdb.hpp>

namespace duckdb {

const char *TryGetEnv(const char *name) {
  const char *res = std::getenv(name);
  if (res) {
    return res;
  }
  return std::getenv(StringUtil::Upper(name).c_str());
}

std::string GetEnvOrDefault(const char *name, const char *default_value) {
  const char *res = TryGetEnv(name);
  if (res) {
    return res;
  }
  return default_value;
}

uint32_t GetEnvOrDefaultInt(const char *name, uint32_t default_value) {
  const char *res = TryGetEnv(name);
  if (res) {
    return std::atoi(res);
  }
  return default_value;
}

bool IsEnvEnabled(const char *name) {
  const char *res = TryGetEnv(name);
  if (!res) {
    return false;
  }

  auto lc_res = StringUtil::Lower(res);
  return lc_res == "1" || lc_res == "true";
}

} // namespace duckdb
