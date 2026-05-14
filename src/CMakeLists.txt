add_subdirectory(quack)

# flock-original sources (currently just the FlockExtension static-link
# façade implementation). PR-2 grows this list with flock_http_server,
# flock_session, flock_auth, flock_lifecycle, etc.
add_library(
  flock_library OBJECT
  flock_extension.cpp)

# Propagate EXTENSION_SOURCES (populated by quack/, quack/storage/, and
# flock_library above) up to the root CMakeLists.txt scope.
set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES} $<TARGET_OBJECTS:flock_library>
    PARENT_SCOPE)
