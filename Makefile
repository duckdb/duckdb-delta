PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=deltatable
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

ifeq ($(SANITIZER_MODE), thread)
	EXT_DEBUG_FLAGS:=-DENABLE_THREAD_SANITIZER=1
endif

ifneq ("${CUSTOM_LINKER}", "")
	EXT_DEBUG_FLAGS:=${EXT_DEBUG_FLAGS} -DCUSTOM_LINKER=${CUSTOM_LINKER}
endif

# Set test paths
test_release: export DELTA_KERNEL_TESTS_PATH=./build/release/rust/src/delta_kernel/kernel/tests/data
test_release: export DAT_PATH=./build/release/rust/src/delta_kernel/acceptance/tests/dat

test_debug: export DELTA_KERNEL_TESTS_PATH=./build/debug/rust/src/delta_kernel/kernel/tests/data
test_debug: export DAT_PATH=./build/debug/rust/src/delta_kernel/acceptance/tests/dat

# Core extensions that we need for crucial testing
DEFAULT_TEST_EXTENSION_DEPS=tpcds;tpch;json;
# For cloud testing we also need these extensions
FULL_TEST_EXTENSION_DEPS=azure;httpfs;aws

# Aws and Azure have vcpkg dependencies and therefore need vcpkg merging
ifeq (${BUILD_EXTENSION_TEST_DEPS}, full)
	USE_MERGED_VCPKG_MANIFEST:=1
endif

# Set this flag during building to enable the benchmark runner
ifeq (${BUILD_BENCHMARK}, 1)
	TOOLCHAIN_FLAGS:=${TOOLCHAIN_FLAGS} -DBUILD_BENCHMARKS=1
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Include the Makefile from the benchmark directory
include benchmark/benchmark.Makefile

# Generate some test data to test with
# Note: make sure the JAVA_HOME var is set correctly and a venv is configured, e.g:
#   python3 -m venv venv
#	. ./venv/bin/activate
#   export JAVA_HOME=/opt/homebrew/Cellar/openjdk@11/11.0.27/libexec/openjdk.jdk/Contents/Home
generate-data:
	#python3 -m pip install delta-spark duckdb pandas deltalake pyspark typing-extensions pyarrow
	# NOTE: @benfleis - for now pin versions that work, since unversioned/HEAD caused a big JVM stack trace that I couldn't trivially track down;
	# NOTE: bump to v1.5.0 once it's out
	python3 -m pip install delta-spark==4.0.0 deltalake==1.2.1 duckdb==1.5.0.dev166 pandas==2.3.3 pyarrow==22.0.0 pyspark==4.0.1 typing-extensions==4.15.0
	python3 scripts/data_generator/generate_test_data.py

unpack-golden-tables-release:
	./scripts/unwrap_golden_tables.sh

# shortcuts for FFI targets
kernel_debug:
	cd build/debug && cmake --build . --config Debug --target extension/delta/CMakeFiles/delta_kernel

kernel_release:
	cd build/release && cmake --build . --config Release --target extension/delta/CMakeFiles/delta_kernel
