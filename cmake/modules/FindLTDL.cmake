# FindLTDL.cmake
# - Find ltdl header and library
#
# This module defines
#  LTDL_FOUND, if false, do not try to use LTDL.
#  LTDL_INCLUDE_DIRS, where to find ltdl.h.
#  LTDL_LIBRARIES, the libraries to link against to use libltdl.
#
# As a hint allows LTDL_ROOT_DIR

find_path( LTDL_INCLUDE_DIR
    NAMES ltdl.h
    HINTS ${LTDL_ROOT_DIR}/include
    )
find_library( LTDL_LIBRARY
    NAMES ltdl
    HINTS ${LTDL_ROOT_DIR}/lib
    )

include( FindPackageHandleStandardArgs )
find_package_handle_standard_args( LTDL
  FOUND_VAR
    LTDL_FOUND
  REQUIRED_VARS
    LTDL_LIBRARY
    LTDL_INCLUDE_DIR
    )

if ( LTDL_FOUND )
  set( LTDL_INCLUDE_DIRS "${LTDL_INCLUDE_DIR}" )
  set( LTDL_LIBRARIES "${LTDL_LIBRARY}" )
endif ()

mark_as_advanced( LTDL_ROOT_DIR LTDL_INCLUDE_DIR LTDL_LIBRARY )