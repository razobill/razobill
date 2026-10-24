# =============================================================================
# Makefile driving DuckDB community extension CI and C_STRUCT ABI builds
#
# integrates with duckdb/extension-ci-tools to build and package loadable
# extension binaries for community distribution
# ~ drives extension-ci-tools matrix for DuckDB loadable distribution
# =============================================================================
.PHONY: clean clean_all

PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Main extension configuration
EXTENSION_NAME=razobill
EXTENSION_CANONICAL=razobill

# Stable C API (C_STRUCT ABI)
USE_UNSTABLE_C_API=0

# Target DuckDB C API baseline version for ABI metadata
TARGET_DUCKDB_VERSION=v1.5.6
EXTENSION_VERSION?=v$(shell cat VERSION 2>/dev/null | tr -d '[:space:]')
ifeq ($(strip $(EXTENSION_VERSION)),v)
EXTENSION_VERSION=v1.1.6
endif

# Pass build flag to build only the extension in CI
CMAKE_EXTRA_BUILD_FLAGS=-DRAZOBILL_BUILD_EXT_ONLY=ON

all: configure release

# Ensure extension-ci-tools makefiles are included
ifeq ($(wildcard extension-ci-tools/makefiles/c_api_extensions/base.Makefile),)
$(shell git submodule update --init --recursive 2>/dev/null || git clone --depth 1 -b v1.5-variegata https://github.com/duckdb/extension-ci-tools extension-ci-tools 2>/dev/null)
endif

-include extension-ci-tools/makefiles/c_api_extensions/base.Makefile
-include extension-ci-tools/makefiles/c_api_extensions/c_cpp.Makefile

# Multi-config fallback copy step for Windows MSVC if needed
ifeq ($(OS),Windows_NT)
EXTRA_COPY_STEP_RELEASE?=$(PYTHON_VENV_BIN) -c "import os, shutil; src = 'cmake_build/release/Release/$(EXTENSION_LIB_FILENAME)'; dst = '$(OUTPUT_LIB_PATH_RELEASE)'; os.path.exists(src) and shutil.copyfile(src, dst)"
EXTRA_COPY_STEP_DEBUG?=$(PYTHON_VENV_BIN) -c "import os, shutil; src = 'cmake_build/debug/Debug/$(EXTENSION_LIB_FILENAME)'; dst = '$(OUTPUT_LIB_PATH_DEBUG)'; os.path.exists(src) and shutil.copyfile(src, dst)"
endif

configure: venv platform extension_version

debug: build_extension_library_debug build_extension_with_metadata_debug
release: build_extension_library_release build_extension_with_metadata_release

test: test_release
test_debug: test_extension_debug
test_release: test_extension_release

clean: clean_build clean_cmake
clean_all: clean clean_configure
