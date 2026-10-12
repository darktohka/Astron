FIND_PATH(LIBUV_INCLUDE_DIR NAMES uv.h)

# Prefer the static library so the resulting binaries do not depend on
# libuv.so at runtime.  Fall back to the default search (which may pick a
# shared library) if no static library is available.
set(_LIBUV_SAVED_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
FIND_LIBRARY(LIBUV_LIBRARY NAMES uv_a libuv_a uv libuv)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${_LIBUV_SAVED_SUFFIXES})

if(NOT LIBUV_LIBRARY)
	FIND_LIBRARY(LIBUV_LIBRARY NAMES uv_a libuv_a uv libuv)
endif()

get_filename_component(LIBUV_LIBRARY_DIR ${LIBUV_LIBRARY} PATH)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(libuv DEFAULT_MSG
  LIBUV_INCLUDE_DIR
  LIBUV_LIBRARY
  LIBUV_LIBRARY_DIR)

mark_as_advanced(
  LIBUV_INCLUDE_DIR
  LIBUV_LIBRARY_DIR
  LIBUV_LIBRARY)
