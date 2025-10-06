cmake_minimum_required(VERSION 3.13)
project(sidekiq_source)

# SDR++ expects SRC to be defined before including the module cmake.
file(GLOB SRC "src/*.cpp")

# This include creates the target "sidekiq_source" for you.
include(${SDRPP_MODULE_CMAKE})

# ---- SDK root & SUFFIX (overridable) ----
#   -DSidekiq_ROOT=/path/to/sdk
#   -DSUFFIX=x86_64.gcc|msiq-g20g40|z3u|aarch64|...
if (NOT Sidekiq_ROOT)
  if (DEFINED ENV{Sidekiq_DIR} AND NOT "$ENV{Sidekiq_DIR}" STREQUAL "")
    set(Sidekiq_ROOT "$ENV{Sidekiq_DIR}")
  else()
    set(Sidekiq_ROOT "$ENV{HOME}/sidekiq_sdk_current")
  endif()
endif()
file(TO_CMAKE_PATH "${Sidekiq_ROOT}" Sidekiq_ROOT)

if (NOT DEFINED SUFFIX OR SUFFIX STREQUAL "")
  execute_process(COMMAND uname -m OUTPUT_VARIABLE _host_arch OUTPUT_STRIP_TRAILING_WHITESPACE)
  if (_host_arch STREQUAL "x86_64")
    set(SUFFIX "x86_64.gcc")
  elseif (_host_arch MATCHES "aarch64|arm64")
    set(SUFFIX "msiq-g20g40")
  else()
    set(SUFFIX "x86_64.gcc")
  endif()
  if (NOT EXISTS "${Sidekiq_ROOT}/lib/libsidekiq__${SUFFIX}.a")
    file(GLOB _SK_ARCHIVES "${Sidekiq_ROOT}/lib/libsidekiq__*.a")
    if (_SK_ARCHIVES)
      list(GET _SK_ARCHIVES 0 _one)
      get_filename_component(_one_name "${_one}" NAME)
      string(REPLACE "libsidekiq__" "" _sfx "${_one_name}")
      string(REPLACE ".a" "" _sfx "${_sfx}")
      set(SUFFIX "${_sfx}")
    endif()
  endif()
endif()

# ---- Sidekiq headers & lib ----
set(_SK_INC_CAND
  "${Sidekiq_ROOT}/sidekiq_core/inc"
  "${Sidekiq_ROOT}/include"
  /usr/local/include /usr/include)
find_path(Sidekiq_INCLUDE_DIR NAMES sidekiq_api.h PATHS ${_SK_INC_CAND})
if (NOT Sidekiq_INCLUDE_DIR)
  message(FATAL_ERROR "Sidekiq headers not found (expected at ${Sidekiq_ROOT}/sidekiq_core/inc). Set -DSidekiq_ROOT=…")
endif()

find_library(Sidekiq_LIBRARY
  NAMES "libsidekiq__${SUFFIX}.a"
  PATHS "${Sidekiq_ROOT}/lib" /usr/local/lib /usr/lib /usr/lib64)
if (NOT Sidekiq_LIBRARY)
  message(FATAL_ERROR "Sidekiq library libsidekiq__${SUFFIX}.a not found in ${Sidekiq_ROOT}/lib. Check SUFFIX or SDK root.")
endif()

# SDK-bundled deps (glib/tirpc/libusb/z/ffi) live here
set(EPIQ_SUPP_LIB_DIR  "${Sidekiq_ROOT}/lib/support/${SUFFIX}/usr/lib/epiq")
set(EPIQ_PKGCONFIG_DIR "${EPIQ_SUPP_LIB_DIR}/pkgconfig")

# ---- pkg-config (prefer SDK-bundled .pc if present) ----
find_package(PkgConfig QUIET)
if (EXISTS "${EPIQ_PKGCONFIG_DIR}")
  set(ENV{PKG_CONFIG_PATH} "${EPIQ_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")
endif()

# glib-2.0 (required)
if (PkgConfig_FOUND)
  pkg_check_modules(GLIB2 REQUIRED glib-2.0)
else()
  message(FATAL_ERROR "pkg-config not available; needed for glib-2.0")
endif()

# libtirpc (headers required)
if (PkgConfig_FOUND)
  pkg_check_modules(TIRPC QUIET libtirpc)
endif()
if (NOT TIRPC_FOUND)
  set(_TIRPC_LIB_HINTS /usr/lib/epiq /usr/lib /usr/lib64 /usr/local/lib)
  set(_TIRPC_INC_HINTS /usr/include/tirpc /usr/local/include/tirpc /usr/include)
  find_library(TIRPC_LIBRARY NAMES tirpc HINTS ${_TIRPC_LIB_HINTS})
  find_path(TIRPC_INCLUDE_DIR NAMES rpc/rpc.h HINTS ${_TIRPC_INC_HINTS})
  if (TIRPC_LIBRARY AND TIRPC_INCLUDE_DIR)
    set(TIRPC_FOUND TRUE)
    set(TIRPC_LIBRARIES ${TIRPC_LIBRARY})
    set(TIRPC_INCLUDE_DIRS ${TIRPC_INCLUDE_DIR})
    message(STATUS "Found libtirpc (manual): ${TIRPC_LIBRARY}")
  else()
    message(FATAL_ERROR "libtirpc not found (no pkg-config and manual search failed)")
  endif()
endif()

# libusb-1.0 (headers optional for our module)
if (PkgConfig_FOUND)
  pkg_check_modules(LIBUSB QUIET libusb-1.0)
endif()
if (NOT LIBUSB_FOUND)
  set(_USB_LIB_HINTS /usr/lib/epiq /usr/lib /usr/lib64 /usr/local/lib)
  set(_USB_INC_HINTS /usr/include/libusb-1.0 /usr/local/include/libusb-1.0 /usr/include)
  find_library(LIBUSB_LIBRARY
    NAMES usb-1.0 libusb-1.0.so libusb-1.0.so.0
    HINTS ${_USB_LIB_HINTS})
  find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h HINTS ${_USB_INC_HINTS})
  if (LIBUSB_LIBRARY)
    set(LIBUSB_FOUND TRUE)
    set(LIBUSB_LIBRARIES ${LIBUSB_LIBRARY})
    if (LIBUSB_INCLUDE_DIR)
      set(LIBUSB_INCLUDE_DIRS ${LIBUSB_INCLUDE_DIR})
      message(STATUS "Found libusb-1.0 (manual): ${LIBUSB_LIBRARY}")
    else()
      set(LIBUSB_INCLUDE_DIRS "")
      message(WARNING "libusb-1.0 headers not found; proceeding with library only")
    endif()
  else()
    message(FATAL_ERROR "libusb-1.0 not found (no pkg-config and manual search failed)")
  endif()
endif()

# z3u needs libiio (only if you plan to build for z3u)
set(LIBIIO_LIBRARIES "")
set(LIBIIO_INCLUDE_DIRS "")
if (SUFFIX STREQUAL "z3u")
  if (PkgConfig_FOUND)
    pkg_check_modules(LIBIIO QUIET libiio)
  endif()
  if (NOT LIBIIO_FOUND)
    find_library(IIO_LIBRARY NAMES iio iio.so iio.dylib)
    find_path(IIO_INCLUDE_DIR NAMES iio.h PATHS /usr/include /usr/local/include)
    if (IIO_LIBRARY)
      set(LIBIIO_LIBRARIES ${IIO_LIBRARY})
      if (IIO_INCLUDE_DIR)
        set(LIBIIO_INCLUDE_DIRS ${IIO_INCLUDE_DIR})
      endif()
      message(STATUS "Found libiio (manual): ${IIO_LIBRARY}")
    else()
      message(FATAL_ERROR "SUFFIX=z3u requires libiio but it was not found")
    endif()
  else()
    set(LIBIIO_LIBRARIES ${LIBIIO_LIBRARIES})
    set(LIBIIO_INCLUDE_DIRS ${LIBIIO_INCLUDE_DIRS})
    message(STATUS "Found libiio (pkg-config): ${LIBIIO_LIBRARIES}")
  endif()
endif()

# Threads/dl/rt typically needed
find_package(Threads REQUIRED)

# ---------- Augment the existing SDR++ target ----------
# The module target is already created by ${SDRPP_MODULE_CMAKE}
set(MODULE_TARGET sidekiq_source)

target_include_directories(${MODULE_TARGET} PRIVATE
  ${Sidekiq_INCLUDE_DIR}
  ${GLIB2_INCLUDE_DIRS}
  ${TIRPC_INCLUDE_DIRS}
  ${LIBUSB_INCLUDE_DIRS}
  ${LIBIIO_INCLUDE_DIRS}
)

# Prefer the SDK support dir at link time
if (EXISTS "${EPIQ_SUPP_LIB_DIR}")
  link_directories("${EPIQ_SUPP_LIB_DIR}")
endif()

target_link_directories(${MODULE_TARGET} PRIVATE
  ${GLIB2_LIBRARY_DIRS}
  ${TIRPC_LIBRARY_DIRS}
  ${LIBUSB_LIBRARY_DIRS}
)

target_link_libraries(${MODULE_TARGET} PRIVATE
  ${Sidekiq_LIBRARY}
  ${GLIB2_LIBRARIES}
  ${TIRPC_LIBRARIES}
  ${LIBUSB_LIBRARIES}
  ${LIBIIO_LIBRARIES}
  Threads::Threads
  dl
  rt
)

# RPATH so the runtime finds SDK-bundled libs
if (EXISTS "${EPIQ_SUPP_LIB_DIR}")
  set_target_properties(${MODULE_TARGET} PROPERTIES
    BUILD_RPATH   "${EPIQ_SUPP_LIB_DIR}"
    INSTALL_RPATH "${EPIQ_SUPP_LIB_DIR}")
endif()

# Fail build on unresolved symbols
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(${MODULE_TARGET} PRIVATE -Wl,-z,defs)
endif()

# Status lines
message(STATUS "Sidekiq SDK dir:        ${Sidekiq_ROOT}")
message(STATUS "SUFFIX:                 ${SUFFIX}")
message(STATUS "EPIQ support lib dir:   ${EPIQ_SUPP_LIB_DIR}")
message(STATUS "Sidekiq include dir:    ${Sidekiq_INCLUDE_DIR}")
message(STATUS "Sidekiq lib:            ${Sidekiq_LIBRARY}")
message(STATUS "GLib2 libs:             ${GLIB2_LIBRARIES}")
message(STATUS "TIRPC libs:             ${TIRPC_LIBRARIES}")
message(STATUS "LIBUSB libs:            ${LIBUSB_LIBRARIES}")
if (SUFFIX STREQUAL "z3u")
  message(STATUS "LIBIIO libs:            ${LIBIIO_LIBRARIES}")
endif()
