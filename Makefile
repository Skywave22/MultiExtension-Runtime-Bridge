# xbridged — Multi-Extension Runtime Bridge
#
#   make            build the daemon (build/xbridged)
#   make test       build and run the unit + protocol suite
#   make e2e        run the end-to-end test (real daemon, real HTTP site)
#   make js-test    run the js-engine test (real daemon + Node worker)
#   make check      test + e2e
#   make bench      build and run the benchmark
#   make formats    regenerate the JSON mirror of the format table
#   make asan       build with AddressSanitizer+UBSan and run tests
#   make clean
#
# Everything is plain C11 + libc. No third-party build dependency, on any OS,
# which is what lets the same source target Linux, macOS, Windows and (via the
# shared-library ABI) mobile.

CC      ?= cc
CSTD    ?= -std=c11
OPT     ?= -O2
WARN     = -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings \
           -Wno-unused-parameter -Wvla
DEFS     = -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
INC      = -Icore/include

BUILD   ?= build
BIN      = $(BUILD)/xbridged

CORE_SRC = $(wildcard core/src/*.c)
CORE_OBJ = $(patsubst core/src/%.c,$(BUILD)/obj/%.o,$(CORE_SRC))

TEST_SRC = tests/xbtest.c tests/main.c \
           $(wildcard tests/unit/*.c) $(wildcard tests/protocol/*.c)

TOOL_BIN = $(BUILD)/gen_formats
TEST_BIN = $(BUILD)/xbridge_tests
TEST_OBJ = $(patsubst %.c,$(BUILD)/t/%.o,$(TEST_SRC))
TEST_CORE_OBJ = $(filter-out $(BUILD)/obj/main.o,$(CORE_OBJ))

LDLIBS  += -lpthread -lm
ifneq ($(OS),Windows_NT)
  LDLIBS += -lrt
endif

CFLAGS  += $(CSTD) $(OPT) $(WARN) $(DEFS) $(INC) $(SAN_FLAGS) -MMD -MP -fPIC
LDFLAGS += $(SAN_FLAGS)

.PHONY: all clean test bench asan check formats e2e js-test node-test python-test

all: $(BIN)

$(BUILD)/obj/%.o: core/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/t/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -c $< -o $@

$(BIN): $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_OBJ) $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_BIN): $(TEST_OBJ) $(TEST_CORE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(TEST_OBJ) $(TEST_CORE_OBJ) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

bench: $(BUILD)/xbridge_bench
	$(BUILD)/xbridge_bench

# Regenerate the JSON mirror of the format table (checked by test_registry).
formats: $(TOOL_BIN)
	@mkdir -p core/formats
	$(TOOL_BIN) core/formats

$(TOOL_BIN): tools/gen_formats.c $(TEST_CORE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) tools/gen_formats.c $(TEST_CORE_OBJ) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/xbridge_bench: tests/bench/bench.c $(TEST_CORE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) tests/bench/bench.c $(TEST_CORE_OBJ) $(LDFLAGS) $(LDLIBS) -o $@

asan:
	$(MAKE) BUILD=build/asan OPT="-O1 -g" \
	  SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" test

clean:
	rm -rf build

e2e: all
	python3 tests/e2e/run_e2e.py

# Proves the worker-hosted engine path with a real Node process.
js-test: all
	@command -v node >/dev/null 2>&1 || { echo "node not installed; skipping"; exit 0; }
	python3 tests/e2e/js_engine.py

# SDK conformance: each client library must drive a real daemon end to end.
node-test: all
	@command -v node >/dev/null 2>&1 || { echo "node not installed; skipping"; exit 0; }
	node sdk/node/test/smoke.js

python-test: all
	python3 sdk/python/test_smoke.py

check: all test e2e js-test node-test

-include $(CORE_OBJ:.o=.d)
-include $(TEST_OBJ:.o=.d)
