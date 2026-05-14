#pragma once

#include <duckdb.hpp>

namespace duckdb {
bool IsMDConnected(Connection &);
std::string GetMDToken(Connection &);
} // namespace duckdb
