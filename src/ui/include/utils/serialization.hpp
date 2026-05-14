#pragma once

#include "duckdb.hpp"

#include <string>

namespace duckdb {
namespace ui {

struct EmptyResult {
  void Serialize(duckdb::Serializer &serializer) const;
};

struct TokenizeResult {
  duckdb::vector<idx_t> offsets;
  duckdb::vector<duckdb::SimplifiedTokenType> types;

  void Serialize(duckdb::Serializer &serializer) const;
};

struct ColumnNamesAndTypes {
  duckdb::vector<std::string> names;
  duckdb::vector<duckdb::LogicalType> types;

  void Serialize(duckdb::Serializer &serializer) const;
};

struct Chunk {
  uint16_t row_count;
  duckdb::vector<duckdb::Vector> vectors;

  void Serialize(duckdb::Serializer &serializer) const;
};

struct SuccessResult {
  ColumnNamesAndTypes column_names_and_types;
  duckdb::vector<Chunk> chunks;

  void Serialize(duckdb::Serializer &serializer) const;
};

struct ErrorResult {
  std::string error;

  void Serialize(duckdb::Serializer &serializer) const;
};

} // namespace ui
} // namespace duckdb
