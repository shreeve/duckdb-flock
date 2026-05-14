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

# Propagate EXTENSION_SOURCES (populated by quack/, quack/storage/, and
# flock_library above) up to the root CMakeLists.txt scope.
set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES} $<TARGET_OBJECTS:flock_library>
    PARENT_SCOPE)
