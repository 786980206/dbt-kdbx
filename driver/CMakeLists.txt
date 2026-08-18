cmake_minimum_required(VERSION 3.14)
project(adbc-driver-kdbx VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Optimisation: -O3 + LTO where available; strip symbols from release builds.
if(NOT CMAKE_BUILD_TYPE MATCHES "Debug")
  add_compile_options(-O3 -fno-plt)
  add_link_options(-Wl,--gc-sections -Wl,--strip-all)
  add_compile_options(-fdata-sections -ffunction-sections)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_supported)
  if(_ipo_supported)
    add_compile_options(-flto)
    add_link_options(-flto)
  endif()
endif()

# nanoarrow (Arrow C Data Interface + IPC stream parsing)
set(NANOARROW_IPC ON CACHE BOOL "Build nanoarrow IPC extension")
add_subdirectory(
  thirdparty/nanoarrow
  thirdparty/nanoarrow/build
  EXCLUDE_FROM_ALL
)

# ADBC driver shared library
add_library(adbc_driver_kdbx SHARED
  src/adbc_kdbx_driver.c
  src/adbc_kdbx_database.c
  src/adbc_kdbx_connection.c
  src/adbc_kdbx_statement.c
)

target_include_directories(adbc_driver_kdbx
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/nanoarrow/src>
)

target_link_libraries(adbc_driver_kdbx PRIVATE nanoarrow_ipc_static ${CMAKE_SOURCE_DIR}/lib/c.o)

set_target_properties(adbc_driver_kdbx PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
)

include(GNUInstallDirs)
install(TARGETS adbc_driver_kdbx LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/arrow-adbc/adbc.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/arrow-adbc)
