# msh - minimal Unix-style shell
#
# Primary target environment is Linux (including WSL2) with GCC and GNU Make.
# Targets:
#   make / make build  optimised build
#   make debug         -O0 -g3 build for GDB
#   make asan          AddressSanitizer + UndefinedBehaviorSanitizer build
#   make test          build and run unit + integration tests
#   make test-pty      Ctrl+D-at-a-terminal test (Linux only, needs -lutil)
#   make valgrind      memcheck over the execution paths (needs valgrind)
#   make clean         remove build artefacts

# `CC ?= gcc` would not take effect: Make predefines CC as "cc", which counts
# as already set. Override it only when the value is still Make's own default.
ifeq ($(origin CC),default)
CC := gcc
endif
CSTD    := -std=c11
# _POSIX_C_SOURCE exposes the POSIX interfaces (isatty, STDIN_FILENO) that the
# strict C11 mode would otherwise hide.
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
            -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings \
            -Wpointer-arith -Wcast-qual -Wundef -Wvla

# Optimisation/debug level only. Overriding this (as the debug and asan
# targets do) must not discard the language standard or the warning set, so
# those live in CFLAGS separately rather than being bundled into one variable.
OPTFLAGS ?= -O2
CFLAGS   := $(CSTD) $(WARNINGS) $(OPTFLAGS) $(EXTRA_CFLAGS)

BUILD_DIR := out
BIN       := $(BUILD_DIR)/msh

SRCS := src/main.c src/shell.c src/lineread.c src/parser.c src/executor.c src/builtins.c src/redirect.c src/jobs.c src/signals.c
OBJS := $(SRCS:src/%.c=$(BUILD_DIR)/%.o)

# Sources under test, excluding main.c (the test binaries supply their own).
LIB_SRCS  := src/shell.c src/lineread.c src/parser.c src/executor.c src/builtins.c src/redirect.c src/jobs.c src/signals.c
UNIT_SRCS := $(wildcard tests/unit/test_*.c)
UNIT_BINS := $(UNIT_SRCS:tests/unit/test_%.c=$(BUILD_DIR)/test_%)

.PHONY: all build debug asan test test-unit test-integration test-pty valgrind clean

all: build
build: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

debug:
	$(MAKE) clean
	$(MAKE) OPTFLAGS="-O0 -g3 -fno-omit-frame-pointer" build

# ASan/UBSan are supported by GCC and Clang on Linux. They are not available
# in the MinGW-w64 toolchain, where this target is expected to fail at link
# time.
asan:
	$(MAKE) clean
	$(MAKE) OPTFLAGS="-O1 -g3 -fno-omit-frame-pointer" \
	        EXTRA_CFLAGS="-fsanitize=address,undefined" \
	        LDFLAGS="-fsanitize=address,undefined" build test

# Unit test binaries link the shell sources directly.
$(BUILD_DIR)/test_%: tests/unit/test_%.c $(LIB_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $< $(LIB_SRCS)

test-unit: $(UNIT_BINS)
	@fail=0; for t in $(UNIT_BINS); do \
	    echo "== $$t"; ./$$t || fail=1; \
	done; exit $$fail

test-integration: $(BIN)
	@MSH_BIN=$(BIN) sh tests/integration/test_repl.sh
	@MSH_BIN=$(BIN) sh tests/integration/test_exec.sh
	@MSH_BIN=$(BIN) sh tests/integration/test_builtins.sh
	@MSH_BIN=$(BIN) sh tests/integration/test_quoting.sh
	@MSH_BIN=$(BIN) sh tests/integration/test_redirect.sh
	@MSH_BIN=$(BIN) sh tests/integration/test_pipeline.sh
	@MSH_BIN=$(BIN) sh tests/integration/test_jobs.sh

test: test-unit test-integration
	@echo "all tests passed"

# Kept out of `test` because forkpty() and -lutil are Linux-specific.
test-pty: $(BIN) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/test_pty \
	      tests/integration/test_pty.c -lutil
	./$(BUILD_DIR)/test_pty ./$(BIN)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/test_pty_jobs \
	      tests/integration/test_pty_jobs.c -lutil
	./$(BUILD_DIR)/test_pty_jobs ./$(BIN)

clean:
	rm -rf $(BUILD_DIR)

# Memcheck over the execution paths. Separate from `test` because Valgrind is
# not installed everywhere and is slow; it skips cleanly if absent.
valgrind: debug
	@MSH_BIN=$(BIN) sh tests/integration/test_valgrind.sh
