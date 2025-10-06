cmake_minimum_required(VERSION 3.13)
project(sidekiq_source)

# SDR++ expects SRC to be defined before including the module cmake.
file(GLOB SRC "src/*.cpp")

# This include creates the target "sidekiq_source" for you.
include(${SDRPP_MODULE_CMAKE})
set(MODULE_TARGET sidekiq_source)

# ---------------- SDK ROOT (overridable + cached) ----------------
if (NOT DEFINED Sidekiq_ROOT OR "${Sidekiq_ROOT}" STREQUAL "")
  if (DEFINED ENV{Sidekiq_DIR} AND NOT "$ENV{Sidekiq_DIR}" STREQUAL "")
    set(Sidekiq_ROOT "$ENV{Sidekiq_DIR}")
  else()
    set(Sidekiq_ROOT "$ENV{HOME}/sidekiq_sdk_current")
  endif()
endif()
file(TO_CMAKE_PATH "${Sidekiq_ROOT}" Sidekiq_ROOT)
set(Sidekiq_ROOT "${Sidekiq_ROOT}" CACHE PATH "Sidekiq SDK root" FORCE)

# ---------------- SUFFIX (overridable + cached) ------------------
# Prefer aarch64.gcc6.3 (matches your "type aarch64" working pkg), then aarch64, then z3u.
if (NOT DEFINED SUFFIX OR SUFFIX STREQUAL "")
  if (EXISTS "${Sidekiq_ROOT}/lib/libsidekiq__aarch64.gcc6.3.a")
    set(SUFFIX "aarch64.gcc6.3")
  elseif (EXISTS "${Sidekiq_ROOT}/lib/libsidekiq__aarch64.a")
    set(SUFFIX "aarch64")
  elseif (EXISTS "${Sidekiq_ROOT}/lib/libsidekiq__z3u.a")
    set(SUFFIX "z3u")
  else()
    execute_process(COMMAND uname -m OUTPUT_VARIABLE _host_arch OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (_host_arch STREQUAL "x86_64")
      set(SUFFIX "x86_64.gcc")
    elseif (_host_arch MATCHES "aarch64|arm64")
      # try common ARM suffixes
      if (EXISTS "${Sidekiq_ROOT}/lib/libsidekiq__aarch64.gcc6.3.a")
        set(SUFFIX "aarch64.gcc6.3")
      elseif (EXISTS "${Sidekiq_ROOT}/lib/libsidekiq__aarch64.a")
        set(SUFFIX "aarch64")
      else()
        set(SUFFIX "msiq-g20g40")
      endif()
    else()
      set(SUFFIX "x86_64.gcc")
    endif()
    # Last resort: first archive present
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
endif()
set(SUFFIX "${SUFFIX}" CACHE STRING "Sidekiq SDK suffix" FORCE)

# ---------------- Headers & archive ------------------------------
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
  message(FATAL_ERROR "libsidekiq__${SUFFIX}.a not found in ${Sidekiq_ROOT}/lib. Check SUFFIX or SDK root.")
endif()

# Derive support suffix from the actual archive (prevents mismatch).
get_filename_component(_sklib_name "${Sidekiq_LIBRARY}" NAME)   # libsidekiq__*.a
string(REPLACE "libsidekiq__" "" _support_sfx "${_sklib_name}")
string(REPLACE ".a" "" _support_sfx "${_support_sfx}")          # e.g., aarch64.gcc6.3 | z3u

# Compute a simple "type" like your working pkg banner (aarch64/x86_64/z3u/…)
set(_arch_type "${_support_sfx}")
string(REGEX REPLACE "\\..*$" "" _arch_type "${_arch_type}")

# SDK-bundled deps path
set(EPIQ_SUPP_LIB_DIR  "${Sidekiq_ROOT}/lib/support/${_support_sfx}/usr/lib/epiq")
set(EPIQ_PKGCONFIG_DIR "${EPIQ_SUPP_LIB_DIR}/pkgconfig")

# ---------------- pkg-config priming ----------------------------
find_package(PkgConfig QUIET)
if (EXISTS "${EPIQ_PKGCONFIG_DIR}")
  set(ENV{PKG_CONFIG_PATH} "${EPIQ_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")
endif()
if (EXISTS "/usr/lib/epiq/pkgconfig")
  set(ENV{PKG_CONFIG_PATH} "/usr/lib/epiq/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

# ---------------- glib2 / tirpc / libusb ------------------------
if (PkgConfig_FOUND)
  pkg_check_modules(GLIB2 REQUIRED glib-2.0)
else()
  message(FATAL_ERROR "pkg-config not available; needed for glib-2.0")
endif()

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

if (PkgConfig_FOUND)
  pkg_check_modules(LIBUSB QUIET libusb-1.0)
endif()
if (NOT LIBUSB_FOUND)
  set(_USB_LIB_HINTS /usr/lib/epiq /usr/lib /usr/lib64 /usr/local/lib)
  set(_USB_INC_HINTS /usr/include/libusb-1.0 /usr/local/include/libusb-1.0 /usr/include)
  find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0.so libusb-1.0.so.0 HINTS ${_USB_LIB_HINTS})
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

# ---------------- libiio (z3u + ARM SDKs) -----------------------
set(LIBIIO_LIBRARIES "")
set(LIBIIO_INCLUDE_DIRS "")
if (_support_sfx STREQUAL "z3u"
    OR _support_sfx STREQUAL "aarch64"
    OR _support_sfx STREQUAL "aarch64.gcc6.3"
    OR _support_sfx STREQUAL "arm_cortex-a9.gcc7.2.1_gnueabihf")

  if (PkgConfig_FOUND)
    pkg_check_modules(LIBIIO QUIET libiio)
  endif()

  # Fallback if pkg-config fails OR returns no -l flags
  if (NOT LIBIIO_FOUND OR NOT LIBIIO_LIBRARIES)
    find_library(IIO_LIBRARY NAMES iio iio.so iio.dylib
                 HINTS /usr/lib/epiq ${EPIQ_SUPP_LIB_DIR} /usr/lib /usr/lib64 /usr/local/lib)
    find_path(IIO_INCLUDE_DIR NAMES iio.h
              PATHS /usr/include /usr/local/include /usr/lib/epiq/include)
    if (IIO_LIBRARY)
      set(LIBIIO_LIBRARIES ${IIO_LIBRARY})
      if (IIO_INCLUDE_DIR)
        set(LIBIIO_INCLUDE_DIRS ${IIO_INCLUDE_DIR})
      endif()
      message(STATUS "Resolved libiio: ${IIO_LIBRARY}")
    else()
      message(FATAL_ERROR "SDK '${_support_sfx}' requires libiio but it was not found")
    endif()
  else()
    message(STATUS "Found libiio (pkg-config): ${LIBIIO_LIBRARIES}")
  endif()
endif()

# ---------------- Threads / dl / rt ------------------------------
find_package(Threads REQUIRED)

# ---------------- Target wiring ---------------------------------
target_include_directories(${MODULE_TARGET} PRIVATE
  ${Sidekiq_INCLUDE_DIR}
  ${GLIB2_INCLUDE_DIRS}
  ${TIRPC_INCLUDE_DIRS}
  ${LIBUSB_INCLUDE_DIRS}
  ${LIBIIO_INCLUDE_DIRS}
)

# Library search paths
if (EXISTS "${EPIQ_SUPP_LIB_DIR}")
  link_directories("${EPIQ_SUPP_LIB_DIR}")
endif()
if (EXISTS "/usr/lib/epiq")
  target_link_directories(${MODULE_TARGET} PRIVATE "/usr/lib/epiq")
endif()
target_link_directories(${MODULE_TARGET} PRIVATE
  ${GLIB2_LIBRARY_DIRS}
  ${TIRPC_LIBRARY_DIRS}
  ${LIBUSB_LIBRARY_DIRS}
)

# Link order: static sidekiq first, then deps
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

# RPATH: actual support dir, plus /usr/lib/epiq if present
set(_RPATHS "${EPIQ_SUPP_LIB_DIR}")
if (EXISTS "/usr/lib/epiq")
  list(APPEND _RPATHS "/usr/lib/epiq")
endif()
if (EXISTS "${EPIQ_SUPP_LIB_DIR}")
  set_target_properties(${MODULE_TARGET} PROPERTIES
    BUILD_RPATH   "${_RPATHS}"
    INSTALL_RPATH "${_RPATHS}")
endif()

# Fail build on unresolved symbols
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(${MODULE_TARGET} PRIVATE -Wl,-z,defs)
endif()

# ---------------- Banner (matches your working pkg) --------------
message(STATUS "")
message(STATUS "-- sdk is: ${Sidekiq_ROOT}")
message(STATUS "-- type ${_arch_type}")
message(STATUS "")
message(STATUS "-- Sidekiq_INCLUDE_DIRS - ${Sidekiq_INCLUDE_DIR}")
message(STATUS "-- Sidekiq_LIBRARIES - ${Sidekiq_LIBRARY}")
if (LIBIIO_LIBRARIES)
  message(STATUS "-- OTHER_LIBS - ${LIBIIO_LIBRARIES}")
endif()
message(STATUS "-- Using install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")

