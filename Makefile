prefix ?= /usr/local
BUILD_ROOT ?= build
build_dir ?= $(BUILD_ROOT)/static
build_shared_dir ?= $(BUILD_ROOT)/shared
arduino_export_root ?= $(BUILD_ROOT)/arduino
arduino_library_name ?= Feather
arduino_export_dir ?= $(arduino_export_root)/$(arduino_library_name)
FEATHER_VERSION ?= 0.1.0
FEATHER_REPO_URL ?= https://github.com/kyooni18/Feather
FEATHER_ARDUINO_AUTHOR ?= Feather contributors
FEATHER_ARDUINO_MAINTAINER ?= Feather contributors
FEATHER_ARDUINO_SENTENCE ?= Lightweight cooperative scheduler for embedded C and Arduino projects.
FEATHER_ARDUINO_PARAGRAPH ?= Feather provides priority queues, delayed execution, and repeating tasks.
FEATHER_ARDUINO_CATEGORY ?= Timing
FEATHER_ARDUINO_ARCHITECTURES ?= *
FEATHER_ARDUINO_INCLUDES ?= Feather.h
legacy_build_dirs := build-universal build-shared build-shared-universal build-shared-install

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
feather_cmake_args := -DCMAKE_OSX_ARCHITECTURES=arm64\;x86_64
shared_runtime_env = DYLD_LIBRARY_PATH=$(abspath $(build_shared_dir)):$$DYLD_LIBRARY_PATH
else
feather_cmake_args :=
shared_runtime_env = LD_LIBRARY_PATH=$(abspath $(build_shared_dir)):$$LD_LIBRARY_PATH
endif

cmake_configure_static = cmake -S . -B $(build_dir) $(feather_cmake_args)
cmake_configure_shared = cmake -S . -B $(build_shared_dir) -DFEATHER_BUILD_SHARED=ON $(feather_cmake_args)

.DEFAULT_GOAL := build

.PHONY: all build shared demo fast-demo test-cli memory-test system-score-test tracking-test run run-demo run-fast-demo run-test-cli run-memory-test run-system-score-test run-tracking-test check check-memory check-shared test install install-shared arduino-export clean distclean full-system-benchmark run-full-system-benchmark

all: build

build: $(build_dir)/CMakeCache.txt
	cmake --build $(build_dir)

shared: $(build_shared_dir)/CMakeCache.txt
	cmake --build $(build_shared_dir)

demo:
	cmake -S . -B $(build_dir) -DFEATHER_BUILD_DEMO=ON $(feather_cmake_args)
	cmake --build $(build_dir) --target feather_demo

fast-demo:
	cmake -S . -B $(build_dir) -DFEATHER_BUILD_FAST_DEMO=ON $(feather_cmake_args)
	cmake --build $(build_dir) --target feather_fast_demo

test-cli: $(build_dir)/CMakeCache.txt
	cmake --build $(build_dir) --target feather_test_cli

memory-test:
	cmake -S . -B $(build_dir) -DFEATHER_BUILD_MEMORY_TEST=ON $(feather_cmake_args)
	cmake --build $(build_dir) --target feather_memory_test

system-score-test:
	cmake -S . -B $(build_dir) -DFEATHER_BUILD_SYSTEM_SCORE_TEST=ON $(feather_cmake_args)
	cmake --build $(build_dir) --target feather_system_score_test

tracking-test: $(build_dir)/CMakeCache.txt
	cmake --build $(build_dir) --target feather_resource_tracking_test

run: build
	./$(build_dir)/Feather

run-demo: demo
	./$(build_dir)/FeatherDemo

run-fast-demo: fast-demo
	./$(build_dir)/FeatherFastDemo

run-test-cli: test-cli
	./$(build_dir)/FeatherTestCLI all

run-memory-test: memory-test
	./$(build_dir)/FeatherMemoryTest

run-system-score-test: system-score-test
	./$(build_dir)/FeatherSystemScoreTest $(SYSTEM_SCORE_ARGS)

run-tracking-test: tracking-test
	./$(build_dir)/FeatherResourceTrackingTest

check: build
	./$(build_dir)/Feather
	./$(build_dir)/FeatherTestCLI all
	./$(build_dir)/FeatherResourceTrackingTest

check-memory: run-memory-test

check-shared: shared
	$(shared_runtime_env) ./$(build_shared_dir)/Feather
	$(shared_runtime_env) ./$(build_shared_dir)/FeatherTestCLI all
	$(shared_runtime_env) ./$(build_shared_dir)/FeatherResourceTrackingTest

test: check

install: build
	cmake --install $(build_dir) --prefix $(prefix)

install-shared: shared
	cmake --install $(build_shared_dir) --prefix $(prefix)

arduino-export:
	@set -eu; \
		name='$(arduino_library_name)'; \
		root='$(abspath $(arduino_export_root))'; \
		target='$(abspath $(arduino_export_dir))'; \
		[ -n "$$name" ] || { echo "Error: arduino_library_name must not be empty"; exit 1; }; \
		case "$$name" in *"/"*|*".."*|.|..) echo "Error: arduino_library_name must not contain path separators or traversal segments"; exit 1 ;; esac; \
		[ "$$target" = "$$root/$$name" ] || { echo "Error: arduino_export_dir must resolve to <arduino_export_root>/<arduino_library_name>"; exit 1; }; \
		cmake -E rm -rf "$$target"; \
		cmake -E make_directory "$$target/src"; \
		cmake -E copy_directory System "$$target/src"; \
		# Emit Arduino library metadata (https://arduino.github.io/arduino-cli/latest/library-specification/). \
		printf '%s\n' \
			'name=$(arduino_library_name)' \
			'version=$(FEATHER_VERSION)' \
			'author=$(FEATHER_ARDUINO_AUTHOR)' \
			'maintainer=$(FEATHER_ARDUINO_MAINTAINER)' \
			'sentence=$(FEATHER_ARDUINO_SENTENCE)' \
			'paragraph=$(FEATHER_ARDUINO_PARAGRAPH)' \
			'category=$(FEATHER_ARDUINO_CATEGORY)' \
			'url=$(FEATHER_REPO_URL)' \
			'architectures=$(FEATHER_ARDUINO_ARCHITECTURES)' \
			'includes=$(FEATHER_ARDUINO_INCLUDES)' \
			> "$$target/library.properties"

$(build_dir)/CMakeCache.txt:
	$(cmake_configure_static)

$(build_shared_dir)/CMakeCache.txt:
	$(cmake_configure_shared)

clean:
	cmake -E rm -rf $(BUILD_ROOT) $(legacy_build_dirs)

distclean: clean
	cmake -E rm -rf logs

full-system-benchmark:
	cmake -S . -B build/static -DFEATHER_BUILD_FULL_SYSTEM_BENCHMARK=ON
	cmake --build build/static --target feather_full_system_benchmark

run-full-system-benchmark: full-system-benchmark
	./build/static/FeatherFullSystemBenchmark
