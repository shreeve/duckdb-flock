#include "utils/md_helpers.hpp"

#include <duckdb.hpp>
#include <iostream>

namespace duckdb {
std::string GetMDToken(Connection &connection) {
  if (!IsMDConnected(connection)) {
    return ""; // UI expects an empty response if MD isn't connected
  }

  auto query_res = connection.Query("CALL GET_MD_TOKEN()");
  if (query_res->HasError()) {
    query_res->ThrowError();
    return ""; // unreachable
  }

  auto chunk = query_res->Fetch();
  return chunk->GetValue(0, 0).GetValue<std::string>();
}

bool IsMDConnected(Connection &con) {
  if (!con.context->db->ExtensionIsLoaded("motherduck")) {
    return false;
  }

  auto query_res = con.Query("CALL MD_IS_CONNECTED()");
  if (query_res->HasError()) {
    std::cerr << "Error in IsMDConnected: " << query_res->GetError()
              << std::endl;
    return false;
  }

  auto chunk = query_res->Fetch();
  return chunk->GetValue(0, 0).GetValue<bool>();
}
} // namespace duckdb
