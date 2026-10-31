CXX      := clang++
SAN      :=
CXXFLAGS := -std=c++23 -Wall -Wextra -O2 -Iinclude -MMD -MP $(SAN)
BUILD    := build
SERVER   := server

CORE_SRC := src/resp.cpp src/store.cpp src/commands.cpp
CORE_OBJ := $(CORE_SRC:src/%.cpp=$(BUILD)/%.o)
SRV_OBJ  := $(BUILD)/server.o $(BUILD)/main.o
HEADERS  := $(wildcard include/*.hpp)
TESTS    := resp heap lru store
TEST_BIN := $(TESTS:%=$(BUILD)/test_%)

all: $(SERVER)

$(SERVER): $(CORE_OBJ) $(SRV_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Unit tests link only the core (no sockets).
$(BUILD)/test_%: tests/test_%.cpp tests/check.hpp $(CORE_OBJ) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(CORE_OBJ) -o $@

test-unit: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "== $$t"; ./$$t || exit 1; done

test-protocol: $(SERVER)
	python3 tests/test_protocol.py ./$(SERVER)

test: test-unit test-protocol

# Same tests, built with AddressSanitizer + UBSan (separate build folder and binary).
asan:
	$(MAKE) BUILD=build-asan SERVER=build-asan/server \
	  SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g" test

bench: $(SERVER)
	tests/bench.sh ./$(SERVER)

clean:
	rm -rf build build-asan server

.PHONY: all test test-unit test-protocol asan bench clean

-include $(wildcard $(BUILD)/*.d)
