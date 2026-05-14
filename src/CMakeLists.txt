add_subdirectory(quack)

# flock-original sources. PR-2 added the FlockHttpServer / SessionManager /
# AuthManager / AdminHandlers / lifecycle plumbing.
add_library(
  flock_library OBJECT
  flock_extension.cpp
  flock_http_server.cpp
  flock_session.cpp
  flock_auth.cpp
  admin_handlers.cpp
  flock_lifecycle.cpp)

# PR-3: vendored duckdb-ui sources, refactored into UiHandlers.
# - http_server.cpp deleted (replaced by ui_handlers.cpp)
# - ui_extension.cpp EXCLUDED — its DUCKDB_CPP_EXTENSION_ENTRY(ui, ...)
#   would conflict with our flock_init entry; we absorb its settings
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
# flock_library + ui_library above) up to the root CMakeLists.txt scope.
set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES} $<TARGET_OBJECTS:flock_library> $<TARGET_OBJECTS:ui_library>
    PARENT_SCOPE)
