FIND_PATH(LIBUV_INCLUDE_DIR NAMES uv.h)

FIND_LIBRARY(LIBUV_LIBRARY NAMES uv_a libuv_a uv libuv)
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
