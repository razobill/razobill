# =============================================================================
# CMake build configuration for librazobill, Universal C ABI, and DuckDB ext
#
# builds static engine library, shared universal C ABI (razobill_cabi), DuckDB
# loadable C extension (RazobillDuckdb), unit/integration test suites, and bench
# ~ orchestrates native C++20 engine, C ABI, DuckDB, and benchmark builds
# =============================================================================

cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LIBRAZOBILL_PROJECT_VERSION)
string(STRIP "${LIBRAZOBILL_PROJECT_VERSION}" LIBRAZOBILL_PROJECT_VERSION)

project(librazobill VERSION ${LIBRAZOBILL_PROJECT_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
option(BUILD_SHARED_LIBS "Build shared libraries instead of static" OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_compile_definitions(DOCTEST_CONFIG_USE_STD_HEADERS)

option(RAZOBILL_BUILD_EXT_ONLY "Build only the DuckDB loadable extension" OFF)
if(DEFINED EXTENSION_NAME)
  set(RAZOBILL_BUILD_EXT_ONLY ON)
endif()

set(DUCKDB_TARGET_VERSION "v1.5.6" CACHE STRING "Target DuckDB C API ABI version")

option(ENABLE_NATIVE_OPTIMIZATION "Enable CPU native instruction optimizations (-march=native)" OFF)
option(ENABLE_SANITIZER_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_SANITIZER_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_COVERAGE "Enable code coverage instrumentation (GCC/Clang)" OFF)
option(ENABLE_FUZZING "Build libFuzzer fuzz targets" OFF)
option(ENABLE_LIFETIME_SAFETY "Enable Clang Lifetime Safety Analysis (-Wlifetime-safety)" OFF)
option(RZ_WERROR "Treat compiler warnings as errors" OFF)

if(WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

if(RZ_WERROR)
  if(MSVC)
    add_compile_options(/WX)
  else()
    add_compile_options(-Werror)
  endif()
endif()

if(ENABLE_LIFETIME_SAFETY)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wlifetime-safety -Wlifetime-safety-suggestions -Wlifetime-safety-validations)
  else()
    message(WARNING "ENABLE_LIFETIME_SAFETY requires Clang; current compiler is ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

if(MSVC)
  add_compile_options(/W4 /wd4996)
  if(ENABLE_NATIVE_OPTIMIZATION)
    add_compile_options(/arch:AVX2)
  endif()
  if(ENABLE_SANITIZER_ASAN_UBSAN)
    add_compile_options(/fsanitize=address)
  endif()
elseif(APPLE)
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Woverloaded-virtual)
  if(ENABLE_SANITIZER_ASAN_UBSAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  endif()
  if(ENABLE_SANITIZER_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
  endif()
  if(ENABLE_COVERAGE)
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Woverloaded-virtual)
  if(ENABLE_NATIVE_OPTIMIZATION)
    add_compile_options(-march=native)
  endif()
  if(ENABLE_SANITIZER_ASAN_UBSAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  endif()
  if(ENABLE_SANITIZER_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
  endif()
  if(ENABLE_COVERAGE)
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
  endif()
endif()

# Core library: librazobill (STATIC engine for C++ tests and embedders)
add_library(librazobill STATIC
  src/Slots.cpp
  src/Stats.cpp
  src/Channel.cpp
  src/Dialect.cpp
  src/BackendAdbc.cpp
  src/Razobill.cpp
  src/RazobillC.cpp
)
add_library(librazobill::librazobill ALIAS librazobill)
set_target_properties(librazobill PROPERTIES OUTPUT_NAME razobill_static ARCHIVE_OUTPUT_NAME razobill_static)

target_include_directories(librazobill
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    thirdparty/adbc
    thirdparty/duckdbcapi
)
target_link_libraries(librazobill PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(librazobill PUBLIC LIBRAZOBILL_STATIC)

# Universal C ABI Shared Library
# (for foreign FFIs: Go, Rust, Python ctypes, Zig, C#)
if(NOT RAZOBILL_BUILD_EXT_ONLY)
  add_library(razobill_cabi SHARED src/RazobillC.cpp)
  target_include_directories(razobill_cabi PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_compile_definitions(razobill_cabi PRIVATE LIBRAZOBILL_EXPORTS)
  target_link_libraries(razobill_cabi PRIVATE librazobill)
  if(NOT MSVC)
    target_compile_options(razobill_cabi PRIVATE -fvisibility=hidden)
  endif()
  if(WIN32)
    set_target_properties(razobill_cabi PROPERTIES PREFIX "")
  endif()
  set_target_properties(razobill_cabi PROPERTIES
    OUTPUT_NAME razobill_cabi
    ARCHIVE_OUTPUT_NAME razobill_cabi
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}"
  )
endif()

add_library(RazobillDuckdb SHARED ext/duckdb/DuckdbExtension.cpp)
target_include_directories(RazobillDuckdb PRIVATE include thirdparty/duckdbcapi)
if(DEFINED EXTENSION_NAME)
  target_compile_definitions(RazobillDuckdb PRIVATE DUCKDB_EXTENSION_NAME=${EXTENSION_NAME})
else()
  target_compile_definitions(RazobillDuckdb PRIVATE DUCKDB_EXTENSION_NAME=razobill)
endif()
if(NOT MSVC)
  target_compile_options(RazobillDuckdb PRIVATE -fvisibility=hidden -Wno-missing-field-initializers)
endif()
target_link_libraries(RazobillDuckdb PRIVATE librazobill)
if(WIN32)
  if(NOT MSVC)
    target_link_options(RazobillDuckdb PRIVATE "-Wl,--out-implib,${CMAKE_CURRENT_BINARY_DIR}/razobill_duckdb.lib")
  endif()
  set_target_properties(RazobillDuckdb PROPERTIES
    PREFIX ""
    OUTPUT_NAME razobill
    ARCHIVE_OUTPUT_NAME razobill_duckdb
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}"
  )
else()
  set_target_properties(RazobillDuckdb PROPERTIES
    OUTPUT_NAME razobill
    ARCHIVE_OUTPUT_NAME razobill_duckdb
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}"
  )
endif()

if(NOT RAZOBILL_BUILD_EXT_ONLY)
  find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)
  if(Python3_FOUND)
    if(NOT DUCKDB_PLATFORM)
      if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|ARM|aarch64|arm64")
          set(DUCKDB_PLATFORM "windows_arm64")
        else()
          set(DUCKDB_PLATFORM "windows_amd64")
        endif()
      elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|ARM|aarch64|arm64")
          set(DUCKDB_PLATFORM "osx_arm64")
        else()
          set(DUCKDB_PLATFORM "osx_amd64")
        endif()
      else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|ARM|aarch64|arm64")
          set(DUCKDB_PLATFORM "linux_arm64")
        else()
          set(DUCKDB_PLATFORM "linux_amd64")
        endif()
      endif()
    endif()

    # Sign DuckDB extension with ABI metadata
    # (both razobill and librazobill names)
    add_custom_command(TARGET RazobillDuckdb POST_BUILD
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/AppendExtensionMetadata.py -l $<TARGET_FILE:RazobillDuckdb> -n razobill
              -o ${CMAKE_BINARY_DIR}/razobill.duckdb_extension -p ${DUCKDB_PLATFORM} -dv ${DUCKDB_TARGET_VERSION} -ev v${LIBRAZOBILL_PROJECT_VERSION} --abi-type C_STRUCT
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/razobill.duckdb_extension ${CMAKE_BINARY_DIR}/librazobill.duckdb_extension
      COMMENT "Signing DuckDB loadable extension metadata for community distribution" VERBATIM)

    # Python Bindings: pyrazobill via pybind11
    execute_process(
      COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
      OUTPUT_VARIABLE pybind11_DIR
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    find_package(pybind11 CONFIG QUIET)

    if(pybind11_FOUND)
      pybind11_add_module(Core lib/python/PyRazobill.cpp)
      target_include_directories(Core PRIVATE include)
      target_link_libraries(Core PRIVATE librazobill)
      if(NOT MSVC)
        target_compile_options(Core PRIVATE -Wno-missing-field-initializers)
      endif()
      if(WIN32 AND MINGW)
        target_link_options(Core PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic -l:libwinpthread.a -Wl,-Bdynamic)
      endif()
    endif()
  endif()

  # Testing Suites
  enable_testing()

  add_executable(UnitTests
    tests/UnitCore.cpp
    tests/UnitStats.cpp
    tests/UnitArena.cpp
    tests/UnitCAPI.cpp
    tests/UnitChannel.cpp
    tests/UnitLifetime.cpp
  )
  target_include_directories(UnitTests PRIVATE thirdparty include)
  if(NOT MSVC)
    target_compile_options(UnitTests PRIVATE -Wno-missing-field-initializers)
  endif()
  target_link_libraries(UnitTests PRIVATE librazobill)
  add_test(NAME unit COMMAND UnitTests)

  add_executable(IntegrationDuckdb tests/IntegrationDuckdb.cpp)
  target_include_directories(IntegrationDuckdb PRIVATE thirdparty include tests)
  if(NOT MSVC)
    target_compile_options(IntegrationDuckdb PRIVATE -Wno-missing-field-initializers)
  endif()
  target_link_libraries(IntegrationDuckdb PRIVATE librazobill)
  add_test(NAME integration_duckdb COMMAND IntegrationDuckdb)
  if(Python3_FOUND)
    execute_process(
      COMMAND ${Python3_EXECUTABLE} -c "import _duckdb; print(_duckdb.__file__)"
      OUTPUT_VARIABLE DUCKDB_LIB_PATH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
  endif()
  if(DUCKDB_LIB_PATH)
    set_tests_properties(integration_duckdb PROPERTIES ENVIRONMENT "RZB_EXT=${CMAKE_BINARY_DIR}/librazobill.duckdb_extension;RZB_DUCKDB_LIB=${DUCKDB_LIB_PATH}")
  else()
    set_tests_properties(integration_duckdb PROPERTIES ENVIRONMENT "RZB_EXT=${CMAKE_BINARY_DIR}/librazobill.duckdb_extension")
  endif()

  add_executable(IntegrationChdb tests/IntegrationChdb.cpp)
  target_include_directories(IntegrationChdb PRIVATE thirdparty include tests)
  if(NOT MSVC)
    target_compile_options(IntegrationChdb PRIVATE -Wno-missing-field-initializers)
  endif()
  target_link_libraries(IntegrationChdb PRIVATE librazobill)
  add_test(NAME integration_chdb COMMAND IntegrationChdb)

  add_executable(BenchCore bench/BenchCore.cpp)
  target_include_directories(BenchCore PRIVATE include)
  if(NOT MSVC)
    target_compile_options(BenchCore PRIVATE -Wno-missing-field-initializers)
  endif()
  target_link_libraries(BenchCore PRIVATE librazobill)

  add_executable(BenchStorage bench/BenchStorage.cpp)
  target_include_directories(BenchStorage PRIVATE thirdparty include tests)
  if(NOT MSVC)
    target_compile_options(BenchStorage PRIVATE -Wno-missing-field-initializers)
  endif()
  target_link_libraries(BenchStorage PRIVATE librazobill)

  if(ENABLE_FUZZING)
    add_executable(FuzzDecomposeReconstruct tests/fuzz/FuzzDecomposeReconstruct.cpp)
    target_include_directories(FuzzDecomposeReconstruct PRIVATE thirdparty include)
    target_link_libraries(FuzzDecomposeReconstruct PRIVATE librazobill)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_definitions(FuzzDecomposeReconstruct PRIVATE LIBRAZOBILL_USE_LIBFUZZER=1)
      target_compile_options(FuzzDecomposeReconstruct PRIVATE -fsanitize=fuzzer,address,undefined)
      target_link_options(FuzzDecomposeReconstruct PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
  endif()
endif()

# ==============================================================================
# Public Installation & Packaging (CMake Export, pkg-config, CPack)
# ==============================================================================

# Install librazobill static library and razobill_cabi shared library
if(TARGET razobill_cabi)
  install(TARGETS librazobill razobill_cabi
    EXPORT librazobillTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
else()
  install(TARGETS librazobill
    EXPORT librazobillTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
endif()

# Install public headers
install(DIRECTORY include/librazobill
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install DuckDB extension if built
install(FILES "${CMAKE_BINARY_DIR}/librazobill.duckdb_extension"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/razobill/extensions"
  OPTIONAL
)

# CMake Package Export
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/librazobillConfigVersion.cmake"
  VERSION ${LIBRAZOBILL_PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/librazobillConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/librazobillConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/librazobill"
)

install(EXPORT librazobillTargets
  FILE librazobillTargets.cmake
  NAMESPACE librazobill::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/librazobill"
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/librazobillConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/librazobillConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/librazobill"
)

# pkg-config generation
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/librazobill.pc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/librazobill.pc"
  @ONLY
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/librazobill.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

# CPack binary distribution configuration
set(CPACK_PACKAGE_NAME "librazobill")
set(CPACK_PACKAGE_VENDOR "M. M. Atta and Razobill Contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "High-performance C++20 log parsing and analytical database compaction engine")
set(CPACK_PACKAGE_VERSION ${LIBRAZOBILL_PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
include(CPack)
