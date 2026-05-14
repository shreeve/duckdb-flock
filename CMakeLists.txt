cmake_minimum_required(VERSION 3.5)

# Build structure adapted from duckdb-quack and duckdb-ui, both MIT licensed.
# See LICENSE for the derivative-work notice.

set(TARGET_NAME flock)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# PR-3: flock now uses the OpenSSL-enabled cpp-httplib build
# (duckdb_httplib_openssl namespace). FlockHttpServer's plain HTTP
# listening doesn't actually USE TLS today, but the openssl variant is
# what upstream duckdb-ui handlers use, so we standardize on it
# throughout for one-namespace simplicity (see header comment in
# src/include/flock_http_server.hpp). This requires linking OpenSSL
# explicitly; mirrors duckdb-ui's CMakeLists pattern.
find_package(OpenSSL REQUIRED)

include_directories(
    src/include
    src/quack/include
    src/ui/include
    ${OPENSSL_INCLUDE_DIR}
    duckdb/third_party/httplib
    duckdb/extension/autocomplete/include
)
add_subdirectory(src)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
