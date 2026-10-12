# - Try to find libpqxx (PostgreSQL C++ client library)
#
# libpqxx versions 7.0 and later ship their own CMake config package, which
# is preferred when available.  Older versions are found via pkg-config, and
# as a last resort by searching for the headers and library directly.
#
# Sets the following variables:
#   LIBPQXX_FOUND           - true if libpqxx was found
#   LIBPQXX_INCLUDE_DIRS    - include directories for libpqxx
#   LIBPQXX_LIBRARIES       - libraries to link against (may be an imported target)
#   LIBPQXX_LIBRARY_DIRS    - link directories for libpqxx

# Prefer linking libpq statically so the resulting binaries do not depend on
# libpq.so at runtime.  FindPostgreSQL honours a pre-set PostgreSQL_LIBRARY,
# and libpqxx's config package picks that up through find_dependency().
set(_LIBPQXX_SAVED_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
find_library(_LIBPQ_STATIC_LIBRARY NAMES pq libpq PATH_SUFFIXES lib lib64)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${_LIBPQXX_SAVED_LIBRARY_SUFFIXES})
if(_LIBPQ_STATIC_LIBRARY)
	set(PostgreSQL_LIBRARY "${_LIBPQ_STATIC_LIBRARY}")
endif()

if(NOT libpqxx_FOUND)
	find_package(libpqxx CONFIG QUIET)
endif()

if(libpqxx_FOUND)
	# The config package defines the imported target libpqxx::pqxx.  It does
	# not propagate the libpq dependency, so link it explicitly (the config
	# package itself requires PostgreSQL, so it is guaranteed to be found).
	find_package(PostgreSQL QUIET)
	set(LIBPQXX_LIBRARIES libpqxx::pqxx)
	if(PostgreSQL_FOUND)
		list(APPEND LIBPQXX_LIBRARIES PostgreSQL::PostgreSQL)
		# A static libpq.a does not embed the PostgreSQL common/port helper
		# code, so link those archives (and libm/threads) as well.
		#
		# Use the *_shlib archives: libpq.so links these too, and they are the
		# ones exporting the public pg_char_to_encoding()/pg_encoding_to_char()
		# symbols.  The non-shlib libpgcommon.a is built with
		# -DUSE_PRIVATE_ENCODING_FUNCS and only exports the *_private variants.
		if(_LIBPQ_STATIC_LIBRARY)
			set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
			find_library(_LIBPGCOMMON_STATIC_LIBRARY
				NAMES pgcommon_shlib libpgcommon_shlib pgcommon libpgcommon)
			find_library(_LIBPGPORT_STATIC_LIBRARY
				NAMES pgport_shlib libpgport_shlib pgport libpgport)
			set(CMAKE_FIND_LIBRARY_SUFFIXES ${_LIBPQXX_SAVED_LIBRARY_SUFFIXES})
			if(_LIBPGCOMMON_STATIC_LIBRARY)
				list(APPEND LIBPQXX_LIBRARIES ${_LIBPGCOMMON_STATIC_LIBRARY})
			endif()
			if(_LIBPGPORT_STATIC_LIBRARY)
				list(APPEND LIBPQXX_LIBRARIES ${_LIBPGPORT_STATIC_LIBRARY})
			endif()
			find_package(Threads QUIET)
			if(TARGET Threads::Threads)
				list(APPEND LIBPQXX_LIBRARIES Threads::Threads)
			endif()
			list(APPEND LIBPQXX_LIBRARIES m)
		endif()
	endif()
	get_target_property(LIBPQXX_INCLUDE_DIRS libpqxx::pqxx INTERFACE_INCLUDE_DIRECTORIES)
	get_target_property(_LIBPQXX_IMPORTED_LOCATION libpqxx::pqxx IMPORTED_LOCATION)
	if(NOT _LIBPQXX_IMPORTED_LOCATION)
		# Multi-config builds store the location per configuration.
		get_target_property(_LIBPQXX_IMPORTED_CONFIGS libpqxx::pqxx IMPORTED_CONFIGURATIONS)
		list(GET _LIBPQXX_IMPORTED_CONFIGS 0 _LIBPQXX_CONFIG)
		get_target_property(_LIBPQXX_IMPORTED_LOCATION
			libpqxx::pqxx IMPORTED_LOCATION_${_LIBPQXX_CONFIG})
	endif()
	if(_LIBPQXX_IMPORTED_LOCATION)
		get_filename_component(LIBPQXX_LIBRARY_DIRS ${_LIBPQXX_IMPORTED_LOCATION} PATH)
	endif()
	set(LIBPQXX_FOUND TRUE)
	mark_as_advanced(LIBPQXX_INCLUDE_DIRS LIBPQXX_LIBRARIES LIBPQXX_LIBRARY_DIRS)
	return()
endif()

# Fall back to pkg-config:
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
	pkg_check_modules(LIBPQXX QUIET libpqxx)
endif()

if(NOT LIBPQXX_FOUND)
	# Last resort: search for the headers and library directly.
	find_path(LIBPQXX_INCLUDE_DIR
		NAMES pqxx/pqxx
		PATH_SUFFIXES include
	)
	find_library(LIBPQXX_LIBRARY
		NAMES pqxx libpqxx
		PATH_SUFFIXES lib lib64
	)

	include(FindPackageHandleStandardArgs)
	find_package_handle_standard_args(libpqxx DEFAULT_MSG LIBPQXX_LIBRARY LIBPQXX_INCLUDE_DIR)

	if(libpqxx_FOUND)
		set(LIBPQXX_INCLUDE_DIRS ${LIBPQXX_INCLUDE_DIR})
		set(LIBPQXX_LIBRARIES ${LIBPQXX_LIBRARY})
		get_filename_component(LIBPQXX_LIBRARY_DIRS ${LIBPQXX_LIBRARY} PATH)
	endif()

	mark_as_advanced(LIBPQXX_INCLUDE_DIR LIBPQXX_LIBRARY)
endif()
