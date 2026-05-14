add_subdirectory(storage)

add_library(
  quack_library OBJECT
  quack_clear_cache.cpp
  quack_client.cpp
  quack_extension.cpp
  quack_log.cpp
  quack_message.cpp
  quack_scan.cpp
  quack_server.cpp
  quack_start_stop.cpp
  quack_storage.cpp
  quack_uri.cpp
  serialize_quack_message.cpp)
# quack_http_server.cpp dropped in PR-2: HttpQuackServer's role
# (httplib::Server ownership + listener thread) moved up to
# FlockHttpServer (src/flock_http_server.cpp). QuackHandlers (now in
# quack_server.cpp) registers /quack routes against the shared server
# instead of owning its own.
set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES} $<TARGET_OBJECTS:quack_library>
    PARENT_SCOPE)
