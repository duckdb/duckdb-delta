cmake_minimum_required(VERSION 3.5)
include(ExternalProject)

# Core config
set(TARGET_NAME delta)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

include_directories(src/include)

set(EXTENSION_SOURCES
    src/delta_extension.cpp
    src/delta_functions.cpp
    src/delta_log_types.cpp
    src/delta_macros.cpp
    src/delta_utils.cpp
    src/functions/delta_scan/delta_scan.cpp
    src/functions/delta_scan/delta_multi_file_list.cpp
    src/functions/delta_scan/delta_multi_file_reader.cpp
    src/functions/expression_functions.cpp
    src/storage/delta_catalog.cpp
    src/storage/delta_schema_entry.cpp
    src/storage/delta_table_entry.cpp
    src/storage/delta_transaction.cpp
    src/storage/delta_transaction_manager.cpp)

# Custom config TODO: figure out if we really need this?
if(APPLE)
  set(PLATFORM_LIBS
      m
      c
      System
      resolv
      "-framework Corefoundation -framework SystemConfiguration -framework Security"
  )
elseif(UNIX)
  set(PLATFORM_LIBS m c resolv)
elseif(WIN32)
  set(PLATFORM_LIBS
      ntdll
      crypt32
      ncrypt
      secur32
      ws2_32
      userenv
      bcrypt
      msvcrt
      advapi32
      RuntimeObject)
else()
  message(STATUS "UNKNOWN OS")
endif()

# Setup delta-kernel-rs dependency
set(KERNEL_NAME delta_kernel)

# Set default ExternalProject root directory
set_directory_properties(PROPERTIES EP_PREFIX ${CMAKE_BINARY_DIR}/rust)

set(RUST_ENV_VARS "")

# Propagate arch to rust build for CI
set(RUST_PLATFORM_TARGET "")
if("${OS_NAME}" STREQUAL "linux")
  if("${OS_ARCH}" STREQUAL "arm64")
    set(RUST_PLATFORM_TARGET "aarch64-unknown-linux-gnu")
  elseif("${CMAKE_CXX_COMPILER}" MATCHES "aarch64")
    set(RUST_ENV_VARS
        ${RUST_ENV_VARS}
        CFLAGS_aarch64_unknown_linux_gnu=--sysroot=/usr/aarch64-linux-gnu)
    set(RUST_ENV_VARS
        ${RUST_ENV_VARS}
        CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc)
    set(RUST_ENV_VARS
        ${RUST_ENV_VARS}
        OPENSSL_LIB_DIR=${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib
    )
    set(RUST_ENV_VARS
        ${RUST_ENV_VARS}
        OPENSSL_INCLUDE_DIR=${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include
    )
    set(RUST_PLATFORM_TARGET "aarch64-unknown-linux-gnu")
  else()
    set(RUST_PLATFORM_TARGET "x86_64-unknown-linux-gnu")
  endif()
elseif("${OS_NAME}" STREQUAL "osx")
  if("${OSX_BUILD_ARCH}" STREQUAL "arm64")
    set(RUST_PLATFORM_TARGET "aarch64-apple-darwin")
  elseif("${OSX_BUILD_ARCH}" STREQUAL "x86_64")
    set(RUST_PLATFORM_TARGET "x86_64-apple-darwin")
  elseif("${OS_ARCH}" STREQUAL "arm64")
    set(RUST_PLATFORM_TARGET "aarch64-apple-darwin")
  endif()
elseif(WIN32)
  if(MINGW AND "${OS_ARCH}" STREQUAL "arm64")
    set(RUST_PLATFORM_TARGET "aarch64-pc-windows-gnu")
  elseif(MINGW AND "${OS_ARCH}" STREQUAL "amd64")
    set(RUST_PLATFORM_TARGET "x86_64-pc-windows-gnu")
  elseif(MSVC AND "${OS_ARCH}" STREQUAL "arm64")
    set(RUST_PLATFORM_TARGET "aarch64-pc-windows-msvc")
  elseif(MSVC AND "${OS_ARCH}" STREQUAL "amd64")
    set(RUST_PLATFORM_TARGET "x86_64-pc-windows-msvc")
  endif()
endif()

# We currently only support the predefined targets.
if("${RUST_PLATFORM_TARGET}" STREQUAL "")
  message(FATAL_ERROR "Failed to detect the correct platform")
endif()

set(RUST_PLATFORM_PARAM "--target=${RUST_PLATFORM_TARGET}")
message(STATUS "Building for rust target: ${RUST_PLATFORM_TARGET}")

# Remove whitespaces before and after to prevent messed up env variables
string(STRIP "${RUST_ENV_VARS}" RUST_ENV_VARS)

# Having these set will mess up cross compilation to linux arm
set(RUST_UNSET_ENV_VARS --unset=CC --unset=CXX --unset=LD)

# Define all the relevant delta-kernel-rs paths/names
set(DELTA_KERNEL_LIBNAME
    "${CMAKE_STATIC_LIBRARY_PREFIX}delta_kernel_ffi${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
set(DELTA_KERNEL_LIBPATH
    "${CMAKE_BINARY_DIR}/rust/src/delta_kernel/target/${RUST_PLATFORM_TARGET}/$<IF:$<CONFIG:Debug>,debug,release>/${DELTA_KERNEL_LIBNAME}"
)
set(DELTA_KERNEL_FFI_HEADER_PATH
    "${CMAKE_BINARY_DIR}/rust/src/delta_kernel/target/ffi-headers")
set(DELTA_KERNEL_FFI_HEADER_C
    "${CMAKE_BINARY_DIR}/rust/src/delta_kernel/target/ffi-headers/delta_kernel_ffi.h"
)
set(DELTA_KERNEL_FFI_HEADER_CXX
    "${CMAKE_BINARY_DIR}/rust/src/delta_kernel/target/ffi-headers/delta_kernel_ffi.hpp"
)

# Add rust_example as a CMake target
ExternalProject_Add(
  ${KERNEL_NAME}
  GIT_REPOSITORY "https://github.com/delta-io/delta-kernel-rs"
  # WARNING: the FFI headers are currently pinned due to the C linkage issue of
  # the c++ headers. Currently, when bumping the kernel version, the produced
  # header in ./src/include/delta_kernel_ffi.hpp should be also bumped, applying
  # the fix
  GIT_TAG v0.10.0
  # Prints the env variables passed to the cargo build to the terminal, useful
  # in debugging because passing them through CMake is an error-prone mess
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${RUST_UNSET_ENV_VARS}
                    ${RUST_ENV_VARS} env
  UPDATE_COMMAND ""
  BUILD_IN_SOURCE 1
  # Build debug build
  BUILD_COMMAND
    ${CMAKE_COMMAND} -E env ${RUST_UNSET_ENV_VARS} ${RUST_ENV_VARS} cargo build
    --package delta_kernel_ffi --workspace
    $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:--release> --all-features
    ${RUST_PLATFORM_PARAM}
  # Build DATs
  COMMAND
    ${CMAKE_COMMAND} -E env ${RUST_UNSET_ENV_VARS} ${RUST_ENV_VARS} cargo build
    --manifest-path=${CMAKE_BINARY_DIR}/rust/src/delta_kernel/acceptance/Cargo.toml
  # Define the byproducts, required for building with Ninja
  BUILD_BYPRODUCTS "${DELTA_KERNEL_LIBPATH}"
  BUILD_BYPRODUCTS "${DELTA_KERNEL_FFI_HEADER_C}"
  BUILD_BYPRODUCTS "${DELTA_KERNEL_FFI_HEADER_CXX}"
  INSTALL_COMMAND ""
  LOG_BUILD ON)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# TODO: when C linkage issue is resolved, we should switch back to using the
# generated headers include_directories(${DELTA_KERNEL_FFI_HEADER_PATH})

# Hides annoying linker warnings
set(CMAKE_OSX_DEPLOYMENT_TARGET
    13.3
    CACHE STRING "Minimum OS X deployment version" FORCE)

# Add the default client
add_compile_definitions(DEFINE_DEFAULT_ENGINE)

# Link delta-kernal-rs to static lib
target_link_libraries(${EXTENSION_NAME} ${DELTA_KERNEL_LIBPATH}
                      ${PLATFORM_LIBS})
add_dependencies(${EXTENSION_NAME} delta_kernel)

# Link delta-kernal-rs to dynamic lib
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${DELTA_KERNEL_LIBPATH}
                      ${PLATFORM_LIBS})
add_dependencies(${LOADABLE_EXTENSION_NAME} delta_kernel)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
