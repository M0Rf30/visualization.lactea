#.rst:
# Findlactea
# ----------
# Locates the lactea C library (liblactea_capi) and its generated header
# (lactea.h). lactea is a pure-Rust projectM-compatible music visualizer
# exposing a C ABI; Kodi's depends framework has no cargo target, so the
# library is either found pre-built or compiled from a source checkout with
# the `cargo` toolchain.
#
# Pre-built: set LACTEA_INCLUDE_DIR / LACTEA_LIBRARY, or install the library
# into ADDON_DEPENDS_PATH or a standard search path.
#
# From source: point LACTEA_SOURCE_DIR (CMake variable *or* environment
# variable - Kodi's addon framework does not forward -D options to the addon's
# own configure step, the environment is inherited) at a lactea checkout. This
# module runs `cargo build --release -p lactea-capi` on every configure (a
# no-op when up to date, so the header and library never go stale) and uses
# the result.
#
# A static liblactea_capi.a is preferred: it makes the addon self-contained.
# With only the shared library, a copy outside the system library directories
# is shipped next to the addon binary (see LACTEA_BUNDLE_BINARY) and found
# through an $ORIGIN rpath.
#
# Defines:
#   LACTEA_FOUND
#   LACTEA_INCLUDE_DIRS   - directory containing lactea.h
#   LACTEA_LIBRARIES      - what to link (library plus its system deps)
#   LACTEA_BUNDLE_BINARY  - shared library to install beside the addon, or empty

if(NOT LACTEA_SOURCE_DIR AND DEFINED ENV{LACTEA_SOURCE_DIR})
  set(LACTEA_SOURCE_DIR "$ENV{LACTEA_SOURCE_DIR}")
endif()

set(_lactea_hints)
if(LACTEA_SOURCE_DIR AND EXISTS "${LACTEA_SOURCE_DIR}/Cargo.toml")
  find_program(CARGO_EXECUTABLE cargo)
  if(CARGO_EXECUTABLE)
    message(STATUS "Building lactea-capi from ${LACTEA_SOURCE_DIR} ...")
    execute_process(
      COMMAND ${CARGO_EXECUTABLE} build --release -p lactea-capi
      WORKING_DIRECTORY "${LACTEA_SOURCE_DIR}"
      RESULT_VARIABLE _lactea_build_result)
    if(NOT _lactea_build_result EQUAL 0)
      message(WARNING "cargo build of lactea-capi failed; see the log above")
    endif()
  else()
    message(WARNING "LACTEA_SOURCE_DIR is set but cargo was not found")
  endif()
  list(APPEND _lactea_hints ${LACTEA_SOURCE_DIR}/target/release)
endif()

find_path(LACTEA_INCLUDE_DIR lactea.h
          HINTS ${LACTEA_SOURCE_DIR}/crates/lactea-capi/include
                ${ADDON_DEPENDS_PATH}/include
                ${ADDON_DEPENDS_PATH}/include/lactea
                ${DEPENDS_PATH}/include
                ${DEPENDS_PATH}/include/lactea)

set(_lactea_lib_hints ${_lactea_hints} ${ADDON_DEPENDS_PATH}/lib ${DEPENDS_PATH}/lib)
find_library(LACTEA_STATIC_LIBRARY NAMES liblactea_capi.a HINTS ${_lactea_lib_hints})
find_library(LACTEA_SHARED_LIBRARY NAMES lactea_capi HINTS ${_lactea_lib_hints})

set(LACTEA_BUNDLE_BINARY "")
if(LACTEA_STATIC_LIBRARY)
  set(LACTEA_LIBRARY ${LACTEA_STATIC_LIBRARY})
  set(_lactea_extra_libs ${CMAKE_DL_LIBS} m)
  find_package(Threads)
  if(Threads_FOUND)
    list(APPEND _lactea_extra_libs Threads::Threads)
  endif()
else()
  set(LACTEA_LIBRARY ${LACTEA_SHARED_LIBRARY})
  set(_lactea_extra_libs)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(lactea
                                  REQUIRED_VARS LACTEA_LIBRARY LACTEA_INCLUDE_DIR)

if(LACTEA_FOUND)
  set(LACTEA_LIBRARIES ${LACTEA_LIBRARY} ${_lactea_extra_libs})
  set(LACTEA_INCLUDE_DIRS ${LACTEA_INCLUDE_DIR})

  # A shared library from a system directory is the distro's to provide; any
  # other (a cargo target dir, a depends prefix) must travel with the addon.
  if(NOT LACTEA_STATIC_LIBRARY)
    get_filename_component(_lactea_libdir "${LACTEA_LIBRARY}" DIRECTORY)
    get_filename_component(_lactea_libdir "${_lactea_libdir}" REALPATH)
    set(_lactea_system_dirs /lib /lib64 /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
                            /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE})
    list(FIND _lactea_system_dirs "${_lactea_libdir}" _lactea_in_system)
    if(_lactea_in_system EQUAL -1)
      set(LACTEA_BUNDLE_BINARY ${LACTEA_LIBRARY})
    endif()
  endif()
endif()

mark_as_advanced(LACTEA_INCLUDE_DIR LACTEA_STATIC_LIBRARY LACTEA_SHARED_LIBRARY)
