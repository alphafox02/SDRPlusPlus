cmake_minimum_required(VERSION 3.13)
project(sidekiq_source)

# SDR++ expects SRC to be defined before including the module cmake.
file(GLOB SRC "src/*.cpp")

# Optional verbosity for dependency listings
option(OPT_SIDEKIQ_VERBOSE_DEPS "Verbose Sidekiq dependency logging" OFF)

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
# Choose SDK archive suffix sensibly.
# Order of precedence:
#   1) User-provided -DSUFFIX
#   2) Host-arch-preferred archives in ${Sidekiq_ROOT}/lib
#   3) Any archive that also has a matching support dir in lib/support/<suffix>
#   4) Error out with instructions
if (NOT DEFINED SUFFIX OR SUFFIX STREQUAL "")
  # Normalize host CPU
  if (DEFINED CMAKE_HOST_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _host_cpu)
  else()
    execute_process(COMMAND uname -m OUTPUT_VARIABLE _host_cpu OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(TOLOWER "${_host_cpu}" _host_cpu)
  endif()

  # Preference list by host
  set(_prefer_list "")
  if (_host_cpu MATCHES "x86_64|amd64")
    # Put x86 options FIRST so we don't get tricked by ARM libs bundled in x86 SDKs
    list(APPEND _prefer_list
      "x86_64.gcc"
      "x86_64"
      "z4")      # prefer z4 if present
  elseif (_host_cpu MATCHES "aarch64|arm64")
    list(APPEND _prefer_list
      "aarch64.gcc6.3"
      "aarch64"
      "z4"       # prefer z4 if present
      "arm_cortex-a9.gcc7.2.1_gnueabihf"
      "z3u")
  else()
    # Unknown host—still try common ones
    list(APPEND _prefer_list
      "x86_64.gcc" "x86_64"
      "aarch64.gcc6.3" "aarch64"
      "z4"
      "z3u")
  endif()

  # Try host-preferred options
  set(_found_suffix "")
  foreach(_sfx IN LISTS _prefer_list)
    if (EXISTS "${Sidekiq_ROOT}/lib/libsidekiq__${_sfx}.a")
      set(_found_suffix "${_sfx}")
      break()
    endif()
  endforeach()

  # Fallback: pick any archive that ALSO has a matching support dir (more robust than "first one")
  if (_found_suffix STREQUAL "")
    file(GLOB _sk_archives "${Sidekiq_ROOT}/lib/libsidekiq__*.a")
    foreach(_arc IN LISTS _sk_archives)
      get_filename_component(_nm "${_arc}" NAME_WE)                # libsidekiq__<suffix>
      string(REPLACE "libsidekiq__" "" _sfx "${_nm}")
      if (EXISTS "${Sidekiq_ROOT}/lib/support/${_sfx}")
        set(_found_suffix "${_sfx}")
        break()
      endif()
    endforeach()
  endif()

  if (_found_suffix STREQUAL "")
    message(FATAL_ERROR
      "Could not determine Sidekiq SDK suffix for host '${_host_cpu}'. "
      "Checked in ${Sidekiq_ROOT}/lib. "
      "Please pass -DSUFFIX=<suffix> (e.g. x86_64.gcc, aarch64, z3u, z4).")
  endif()

  set(SUFFIX "${_found_suffix}")
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
string(REPLACE ".a" "" _support_sfx "${_support_sfx}")          # e.g., aarch64.gcc6.3 | z3u | z4

# Compute a simple "type" like your working pkg banner (aarch64/x86_64/z3u/z4/…)
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
  pkg_check_modules(GLIB2 QUIET glib-2.0)
endif()
if (NOT GLIB2_FOUND)
  set(_GLIB2_LIB_HINTS
    "${EPIQ_SUPP_LIB_DIR}"
    /usr/lib/epiq
    /usr/lib
    /usr/lib64
    /usr/local/lib
    /usr/lib/aarch64-linux-gnu
    /usr/lib/x86_64-linux-gnu)
  set(_GLIB2_INC_HINTS
    "${Sidekiq_ROOT}/lib/support/${_support_sfx}/usr/include"
    "${Sidekiq_ROOT}/lib/support/${_support_sfx}/usr/lib"
    "${Sidekiq_ROOT}/lib/support/${_support_sfx}/usr/lib64"
    /usr/include
    /usr/local/include
    /usr/lib/aarch64-linux-gnu
    /usr/lib/x86_64-linux-gnu
    /usr/lib64)

  find_library(GLIB2_LIBRARY
    NAMES glib-2.0 libglib-2.0.so libglib-2.0.so.0
    HINTS ${_GLIB2_LIB_HINTS})
  find_path(GLIB2_INCLUDE_DIR
    NAMES glib.h
    HINTS ${_GLIB2_INC_HINTS}
    PATH_SUFFIXES glib-2.0)
  find_path(GLIB2_CONFIG_INCLUDE_DIR
    NAMES glibconfig.h
    HINTS ${_GLIB2_INC_HINTS}
    PATH_SUFFIXES glib-2.0/include)

  if (GLIB2_LIBRARY)
    set(GLIB2_FOUND TRUE)
    set(GLIB2_LIBRARIES ${GLIB2_LIBRARY})
    get_filename_component(_glib2_libdir "${GLIB2_LIBRARY}" DIRECTORY)
    set(GLIB2_LIBRARY_DIRS "${_glib2_libdir}")
    set(GLIB2_INCLUDE_DIRS "")
    if (GLIB2_INCLUDE_DIR)
      list(APPEND GLIB2_INCLUDE_DIRS "${GLIB2_INCLUDE_DIR}")
    endif()
    if (GLIB2_CONFIG_INCLUDE_DIR)
      list(APPEND GLIB2_INCLUDE_DIRS "${GLIB2_CONFIG_INCLUDE_DIR}")
    endif()
    if (GLIB2_INCLUDE_DIRS)
      list(REMOVE_DUPLICATES GLIB2_INCLUDE_DIRS)
      message(STATUS "Found glib-2.0 (manual): ${GLIB2_LIBRARY}")
    else()
      message(WARNING "glib-2.0 headers not found; proceeding with library only")
    endif()
  else()
    message(FATAL_ERROR "glib-2.0 not found (no pkg-config and manual search failed)")
  endif()
endif()

if (PkgConfig_FOUND)
  pkg_check_modules(TIRPC QUIET libtirpc)
endif()
if (NOT TIRPC_FOUND)
  set(_TIRPC_LIB_HINTS
    "${EPIQ_SUPP_LIB_DIR}"
    /usr/lib/epiq
    /usr/lib
    /usr/lib64
    /usr/local/lib
    /usr/lib/aarch64-linux-gnu
    /usr/lib/x86_64-linux-gnu)
  set(_TIRPC_INC_HINTS /usr/include/tirpc /usr/local/include/tirpc /usr/include)
  find_library(TIRPC_LIBRARY NAMES tirpc HINTS ${_TIRPC_LIB_HINTS})
  find_path(TIRPC_INCLUDE_DIR NAMES rpc/rpc.h HINTS ${_TIRPC_INC_HINTS})
  if (TIRPC_LIBRARY)
    set(TIRPC_FOUND TRUE)
    set(TIRPC_LIBRARIES ${TIRPC_LIBRARY})
    if (TIRPC_INCLUDE_DIR)
      set(TIRPC_INCLUDE_DIRS ${TIRPC_INCLUDE_DIR})
      message(STATUS "Found libtirpc (manual): ${TIRPC_LIBRARY}")
    else()
      set(TIRPC_INCLUDE_DIRS "")
      message(WARNING "libtirpc headers not found; proceeding with library only")
    endif()
  else()
    message(FATAL_ERROR "libtirpc not found (no pkg-config and manual search failed)")
  endif()
endif()

if (PkgConfig_FOUND)
  pkg_check_modules(LIBUSB QUIET libusb-1.0)
endif()
if (NOT LIBUSB_FOUND)
  set(_USB_LIB_HINTS
    "${EPIQ_SUPP_LIB_DIR}"
    /usr/lib/epiq
    /usr/lib
    /usr/lib64
    /usr/local/lib
    /usr/lib/aarch64-linux-gnu
    /usr/lib/x86_64-linux-gnu)
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

# ---------------- Z4-only override (shared .so + bundled gpiod) --
set(_Z4_EXTRA_LIBS "")
if (_support_sfx STREQUAL "z4")
  # Prefer SDK shared sidekiq that matches support bundle
  unset(Sidekiq_LIBRARY CACHE)
  find_library(Sidekiq_LIBRARY
    NAMES sidekiq-dev-z4-1 sidekiq
    HINTS "${EPIQ_SUPP_LIB_DIR}" "${Sidekiq_ROOT}/lib"
    NO_DEFAULT_PATH)
  if (NOT Sidekiq_LIBRARY)
    message(FATAL_ERROR "Z4: shared sidekiq .so not found in ${EPIQ_SUPP_LIB_DIR}; check your SDK install.")
  endif()

  # Ensure linker searches SDK dir BEFORE system
  if (EXISTS "${EPIQ_SUPP_LIB_DIR}")
    target_link_directories(${MODULE_TARGET} BEFORE PRIVATE "${EPIQ_SUPP_LIB_DIR}")
  endif()

  # Resolve libgpiod from SDK bundle (v2)
  find_library(GPIOD_SDK_LIBRARY NAMES gpiod HINTS "${EPIQ_SUPP_LIB_DIR}" NO_DEFAULT_PATH)
  if (NOT GPIOD_SDK_LIBRARY)
    message(FATAL_ERROR "Z4: libgpiod not found in ${EPIQ_SUPP_LIB_DIR}.")
  endif()
  set(_Z4_EXTRA_LIBS "${GPIOD_SDK_LIBRARY}")

  # Runtime RPATH for SDK shared libs
  if (EXISTS "${EPIQ_SUPP_LIB_DIR}")
    set_target_properties(${MODULE_TARGET} PROPERTIES
      BUILD_RPATH   "${EPIQ_SUPP_LIB_DIR}"
      INSTALL_RPATH "${EPIQ_SUPP_LIB_DIR}")
  endif()
endif()

# ---------------- X40 gRPC deps (SDK 4.24+) ----------------------
set(_X40_GRPC_LIBS "")
set(_X40_GRPC_LIB_DIRS "")
set(_X40_EXTRA_LIBS "")
if (_support_sfx STREQUAL "msiq-x40")
  if (PkgConfig_FOUND)
    pkg_check_modules(GRPCPP QUIET grpc++)
    pkg_check_modules(GRPC QUIET grpc)
  endif()

  if (GRPCPP_FOUND)
    list(APPEND _X40_GRPC_LIBS ${GRPCPP_LIBRARIES})
    list(APPEND _X40_GRPC_LIB_DIRS ${GRPCPP_LIBRARY_DIRS})
  endif()
  if (GRPC_FOUND)
    list(APPEND _X40_GRPC_LIBS ${GRPC_LIBRARIES})
    list(APPEND _X40_GRPC_LIB_DIRS ${GRPC_LIBRARY_DIRS})
  endif()

  if (NOT _X40_GRPC_LIBS)
    find_library(GRPCPP_LIBRARY NAMES grpc++ HINTS ${EPIQ_SUPP_LIB_DIR} /usr/lib/epiq NO_DEFAULT_PATH)
    find_library(GRPC_LIBRARY   NAMES grpc   HINTS ${EPIQ_SUPP_LIB_DIR} /usr/lib/epiq NO_DEFAULT_PATH)
    find_library(GPR_LIBRARY    NAMES gpr    HINTS ${EPIQ_SUPP_LIB_DIR} /usr/lib/epiq NO_DEFAULT_PATH)
    find_library(PROTOBUF_LIBRARY NAMES protobuf HINTS ${EPIQ_SUPP_LIB_DIR} /usr/lib/epiq NO_DEFAULT_PATH)
    if (NOT GRPCPP_LIBRARY OR NOT GRPC_LIBRARY OR NOT GPR_LIBRARY)
      message(FATAL_ERROR "X40: gRPC libs not found in ${EPIQ_SUPP_LIB_DIR} (expected grpc++, grpc, gpr)")
    endif()
    list(APPEND _X40_GRPC_LIBS ${GRPCPP_LIBRARY} ${GRPC_LIBRARY} ${GPR_LIBRARY})
    if (PROTOBUF_LIBRARY)
      list(APPEND _X40_GRPC_LIBS ${PROTOBUF_LIBRARY})
    endif()
  endif()

  # Add SDK-bundled deps commonly required by grpc++
  if (EXISTS "${EPIQ_SUPP_LIB_DIR}")
    file(GLOB _X40_GRPC_DEPS
      "${EPIQ_SUPP_LIB_DIR}/libabsl_*.so*"
      "${EPIQ_SUPP_LIB_DIR}/libaddress_sorting.so*"
      "${EPIQ_SUPP_LIB_DIR}/libre2.so*"
      "${EPIQ_SUPP_LIB_DIR}/libupb.so*"
      "${EPIQ_SUPP_LIB_DIR}/libcares.so*"
      "${EPIQ_SUPP_LIB_DIR}/libssl.so*"
      "${EPIQ_SUPP_LIB_DIR}/libcrypto.so*"
      "${EPIQ_SUPP_LIB_DIR}/libprotobuf*.so*"
      "${EPIQ_SUPP_LIB_DIR}/libz.so*")
    if (_X40_GRPC_DEPS)
      list(APPEND _X40_GRPC_LIBS ${_X40_GRPC_DEPS})
    endif()
  endif()

  list(REMOVE_DUPLICATES _X40_GRPC_LIBS)

  # X40 GPIO dependency
  if (EXISTS "${EPIQ_SUPP_LIB_DIR}")
    find_library(GPIOD_SDK_LIBRARY NAMES gpiod HINTS "${EPIQ_SUPP_LIB_DIR}" NO_DEFAULT_PATH)
    if (GPIOD_SDK_LIBRARY)
      list(APPEND _X40_EXTRA_LIBS ${GPIOD_SDK_LIBRARY})
    else()
      message(FATAL_ERROR "X40: libgpiod not found in ${EPIQ_SUPP_LIB_DIR}")
    endif()
  endif()

  # Prefer SDK libs before system
  if (EXISTS "${EPIQ_SUPP_LIB_DIR}")
    target_link_directories(${MODULE_TARGET} BEFORE PRIVATE "${EPIQ_SUPP_LIB_DIR}")
  endif()
endif()

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
  ${_X40_GRPC_LIB_DIRS}
  ${GLIB2_LIBRARY_DIRS}
  ${TIRPC_LIBRARY_DIRS}
  ${LIBUSB_LIBRARY_DIRS}
)

# Link order: sidekiq first, then Z4/X40 extras, then deps
target_link_libraries(${MODULE_TARGET} PRIVATE
  ${Sidekiq_LIBRARY}
  ${_Z4_EXTRA_LIBS}
  ${_X40_GRPC_LIBS}
  ${_X40_EXTRA_LIBS}
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
# Detect host CPU for banner
if (DEFINED CMAKE_HOST_SYSTEM_PROCESSOR)
  string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _host_cpu_banner)
else()
  execute_process(COMMAND uname -m OUTPUT_VARIABLE _host_cpu_banner OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(TOLOWER "${_host_cpu_banner}" _host_cpu_banner)
endif()

message(STATUS "")
message(STATUS "-- sdk is: ${Sidekiq_ROOT}")
message(STATUS "-- host cpu: ${_host_cpu_banner}")
message(STATUS "-- type ${_arch_type}")
message(STATUS "")
message(STATUS "-- Sidekiq_INCLUDE_DIRS - ${Sidekiq_INCLUDE_DIR}")
message(STATUS "-- Sidekiq_LIBRARIES - ${Sidekiq_LIBRARY}")
if (LIBIIO_LIBRARIES)
  message(STATUS "-- OTHER_LIBS - ${LIBIIO_LIBRARIES}")
endif()
if (_support_sfx STREQUAL "z4")
  message(STATUS "-- Z4 extras - ${_Z4_EXTRA_LIBS}")
  message(STATUS "-- RPATH - ${EPIQ_SUPP_LIB_DIR}")
endif()
if (_support_sfx STREQUAL "msiq-x40")
  list(LENGTH _X40_GRPC_LIBS _x40_grpc_lib_count)
  if (OPT_SIDEKIQ_VERBOSE_DEPS)
    message(STATUS "-- X40 gRPC libs - ${_X40_GRPC_LIBS}")
  else()
    message(STATUS "-- X40 gRPC libs - grpc++ grpc gpr protobuf (SDK bundle), resolved ${_x40_grpc_lib_count} entries")
  endif()
endif()
message(STATUS "-- Using install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
