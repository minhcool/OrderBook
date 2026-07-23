CXX ?= g++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -pedantic -Iinclude
PERF_CXXFLAGS ?= $(CXXFLAGS) -O2 -DNDEBUG

BUILD_DIR := build
CORE_SRC := src/orderbook.cpp src/exchange.cpp
TEST_SRC := tests/test_orderbook.cpp
DEMO_SRC := examples/demo.cpp
PERF_SRC := benchmarks/perf_orderbook.cpp
API_SRC := apps/api_server.cpp

TEST_BIN := $(BUILD_DIR)/test_orderbook.exe
DEMO_BIN := $(BUILD_DIR)/orderbook_demo.exe
PERF_BIN := $(BUILD_DIR)/orderbook_perf.exe
API_BIN := $(BUILD_DIR)/orderbook_api.exe
API_LDLIBS ?= -lssl -lcrypto -lws2_32

.PHONY: all test demo perf api-build api-test api clean

all: test demo

$(BUILD_DIR):
	mkdir $(BUILD_DIR)

$(TEST_BIN): $(TEST_SRC) $(CORE_SRC) include/orderbook/*.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(TEST_SRC) $(CORE_SRC) -o $(TEST_BIN)

$(DEMO_BIN): $(DEMO_SRC) $(CORE_SRC) include/orderbook/*.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEMO_SRC) $(CORE_SRC) -o $(DEMO_BIN)

$(PERF_BIN): $(PERF_SRC) $(CORE_SRC) include/orderbook/*.h | $(BUILD_DIR)
	$(CXX) $(PERF_CXXFLAGS) $(PERF_SRC) $(CORE_SRC) -o $(PERF_BIN)

$(API_BIN): $(API_SRC) $(CORE_SRC) include/orderbook/*.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(API_SRC) $(CORE_SRC) -o $(API_BIN) $(API_LDLIBS)

test: $(TEST_BIN)
	$(TEST_BIN)

demo: $(DEMO_BIN)
	$(DEMO_BIN)

perf: $(PERF_BIN)
	$(PERF_BIN)

api-build: $(API_BIN)

api-test: $(API_BIN)
	powershell -ExecutionPolicy Bypass -File tests/test_api_lobbies.ps1
	powershell -ExecutionPolicy Bypass -File tests/test_api_auth.ps1

api: $(API_BIN)
	$(API_BIN)

clean:
	if exist $(BUILD_DIR) rmdir /S /Q $(BUILD_DIR)
