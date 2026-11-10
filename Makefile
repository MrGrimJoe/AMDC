CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
SRC      := src/cm.cpp src/transforms.cpp src/container.cpp
BIN      := build/amdc1

all: $(BIN) build/test_amdc1

build:
	mkdir -p build

$(BIN): $(SRC) src/main.cpp src/amdc1.hpp src/transforms.hpp | build
	$(CXX) $(CXXFLAGS) $(SRC) src/main.cpp -o $@

build/test_amdc1: $(SRC) tests/test_amdc1.cpp src/amdc1.hpp src/transforms.hpp | build
	$(CXX) $(CXXFLAGS) $(SRC) tests/test_amdc1.cpp -o $@

test: all
	./build/test_amdc1
	sh tests/crosscheck.sh

clean:
	rm -rf build

.PHONY: all test clean
