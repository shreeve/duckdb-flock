add_subdirectory(quack)

# harbor-original sources. PR-2 added the HarborHttpServer / SessionManager /
# AuthManager / AdminHandlers / lifecycle plumbing. PR-4 added harbor_crypto
# (libcrypto wrapper for SHA-256, HMAC-SHA256, RAND_bytes, base64url) and
# AuthHandlers (POST /auth/login, POST /auth/logout, OPTIONS /auth/*,
# OPTIONS /quack). PR-5 adds SqlHandlers (POST /sql NDJSON streaming, /sql/sessions
# lifecycle), the per-LogicalTypeId chunk encoder, the param decoder
# (Mode A implicit + Mode B typed wrapper), and the JSON writer used
# by both.
add_library(
  harbor_library OBJECT
  harbor_extension.cpp
  harbor_http_server.cpp
  harbor_session.cpp
  harbor_auth.cpp
  harbor_crypto.cpp
  admin_handlers.cpp
  auth_handlers.cpp
  harbor_lifecycle.cpp
  sql_json_writer.cpp
  sql_chunk_encoder.cpp
  sql_param_decoder.cpp
  sql_handlers.cpp
  harbor_query_timeout.cpp)

# PR-3: vendored duckdb-ui sources, refactored into UiHandlers.
# - http_server.cpp deleted (replaced by ui_handlers.cpp)
# - ui_extension.cpp EXCLUDED — its DUCKDB_CPP_EXTENSION_ENTRY(ui, ...)
#   would conflict with our harbor_init entry; we absorb its settings
#   + storage extension setup into quack_extension.cpp::LoadInternal.
# - watcher.cpp edited to take (db, dispatcher, polling_ms) directly
#   instead of HttpServer& — see docs/upstream-ui-patches.md.
add_library(
  ui_library OBJECT
  ui/event_dispatcher.cpp
  ui/settings.cpp
  ui/state.cpp
  ui/ui_handlers.cpp
  ui/watcher.cpp
  ui/utils/encoding.cpp
  ui/utils/env.cpp
  ui/utils/helpers.cpp
  ui/utils/md_helpers.cpp
  ui/utils/serialization.cpp)

# Propagate EXTENSION_SOURCES (populated by quack/, quack/storage/, and
# harbor_library + ui_library above) up to the root CMakeLists.txt scope.
set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES} $<TARGET_OBJECTS:harbor_library> $<TARGET_OBJECTS:ui_library>
    PARENT_SCOPE)
