# Cmake-free build for the memleak library, demo, and tests.
#
#   make            build the static library, demo, and tests
#   make test       build and run the test suite
#   make demo       build and run the demonstration program
#   make clean      remove all build artifacts
#
# Override the compiler with `make CXX=g++` (defaults to c++).

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
LDFLAGS  ?=
LDLIBS   ?= -pthread

# Export dynamic symbols so reported call sites carry readable names. The flag
# spelling differs between GNU ld (-rdynamic) and Apple ld (-export_dynamic);
# detect Darwin and pick the right one.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    EXPORT_DYNAMIC := -Wl,-export_dynamic
else
    EXPORT_DYNAMIC := -rdynamic
endif

BUILD := build-make
LIB   := $(BUILD)/libmemleak.a

LIB_SRCS  := src/Tracker.cpp src/Backtrace.cpp src/Overrides.cpp
LIB_OBJS  := $(patsubst src/%.cpp,$(BUILD)/%.o,$(LIB_SRCS))

DEMO_BIN  := $(BUILD)/memleak_demo
TEST_BIN  := $(BUILD)/tracker_tests

.PHONY: all test demo clean

all: $(LIB) $(DEMO_BIN) $(TEST_BIN)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(DEMO_BIN): src/demo.cpp $(LIB) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@ $(LDFLAGS) $(EXPORT_DYNAMIC) $(LDLIBS)

$(TEST_BIN): tests/tracker_tests.cpp $(LIB) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@ $(LDFLAGS) $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

demo: $(DEMO_BIN)
	@# The demo exits with the live-allocation count, so ignore its status here.
	-./$(DEMO_BIN)

clean:
	$(RM) -r $(BUILD)
