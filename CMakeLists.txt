cmake_minimum_required(VERSION 3.5)

# Build structure adapted from duckdb-quack and duckdb-ui, both MIT licensed.
# See LICENSE for the derivative-work notice.
#
# PR-1: this CMakeLists is a near-verbatim copy of duckdb-quack's, with the
# extension renamed from "quack" to "flock". The architectural refactor that
# extracts httplib::Server from QuackServer into a shared FlockHttpServer
# lands in PR-2 — see AGENTS.md "Implementation roadmap".

set(TARGET_NAME flock)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

include_directories(
    src/include
    src/quack/include
    duckdb/third_party/httplib
    duckdb/extension/autocomplete/include
)
add_subdirectory(src)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
