add_subdirectory(quack)

# Propagate EXTENSION_SOURCES (populated by quack/ and quack/storage/) up
# to the root CMakeLists.txt scope.
set(EXTENSION_SOURCES ${EXTENSION_SOURCES} PARENT_SCOPE)
