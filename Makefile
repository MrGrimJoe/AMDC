CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread
CM_SRC   := src/cm.cpp src/transforms.cpp src/container.cpp
FAST_SRC := src/fast.cpp
BWT_SRC  := src/bwt.cpp src/bwtcodec.cpp

all: build/amdc1 build/amdcf build/amdcb build/test_amdc1 build/test_fast build/test_bwt build/amdcs build/test_solid

build:
	mkdir -p build

build/amdc1: $(CM_SRC) src/main.cpp src/amdc1.hpp src/transforms.hpp | build
	$(CXX) $(CXXFLAGS) $(CM_SRC) src/main.cpp -o $@

build/amdcf: $(FAST_SRC) src/amdcf_main.cpp src/fast.hpp src/pagecache.hpp | build
	$(CXX) $(CXXFLAGS) $(FAST_SRC) src/amdcf_main.cpp -o $@

build/amdcs: $(FAST_SRC) src/solid.cpp src/solid_main.cpp src/solid.hpp src/fast.hpp | build
	$(CXX) $(CXXFLAGS) $(FAST_SRC) src/solid.cpp src/solid_main.cpp -o $@

build/test_solid: $(FAST_SRC) src/solid.cpp tests/test_solid.cpp src/solid.hpp src/fast.hpp | build
	$(CXX) $(CXXFLAGS) $(FAST_SRC) src/solid.cpp tests/test_solid.cpp -o $@

build/decode_bench: $(FAST_SRC) bench/decode_bench.cpp src/fast.hpp | build
	$(CXX) $(CXXFLAGS) $(FAST_SRC) bench/decode_bench.cpp -o $@

build/amdcb: $(BWT_SRC) src/amdcb_main.cpp src/bwt.hpp src/bwtcodec.hpp | build
	$(CXX) $(CXXFLAGS) $(BWT_SRC) src/amdcb_main.cpp -o $@

build/test_bwt: $(BWT_SRC) tests/test_bwt.cpp src/bwt.hpp src/bwtcodec.hpp | build
	$(CXX) $(CXXFLAGS) $(BWT_SRC) tests/test_bwt.cpp -o $@

build/test_amdc1: $(CM_SRC) tests/test_amdc1.cpp src/amdc1.hpp src/transforms.hpp | build
	$(CXX) $(CXXFLAGS) $(CM_SRC) tests/test_amdc1.cpp -o $@

build/test_fast: $(FAST_SRC) tests/test_fast.cpp src/fast.hpp src/pagecache.hpp | build
	$(CXX) $(CXXFLAGS) $(FAST_SRC) tests/test_fast.cpp -o $@

build/bench_matchcopy: $(FAST_SRC) bench/matchcopy_bench.cpp src/fast.hpp | build
	$(CXX) $(CXXFLAGS) $(FAST_SRC) bench/matchcopy_bench.cpp -o $@

# Synthetic decoder microbenchmark (hot-cache best case, not real-data throughput)
bench-fast: build/bench_matchcopy
	./build/bench_matchcopy 4096

# AMDC-F C++ <-> pure-Python reference interoperability
crosscheck-fast: build/amdcf
	python3 tests/crosscheck_fast.py

test: all
	./build/test_amdc1
	./build/test_fast
	./build/test_solid
	./build/test_bwt
	sh tests/crosscheck.sh
	python3 tests/crosscheck_fast.py

# Memory-safety run of the fast codec (decoder fuzz included)
sanitize: | build
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread $(FAST_SRC) tests/test_fast.cpp -o build/test_fast_san
	./build/test_fast_san
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread $(FAST_SRC) src/solid.cpp tests/test_solid.cpp -o build/test_solid_san
	./build/test_solid_san
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread $(BWT_SRC) tests/test_bwt.cpp -o build/test_bwt_san
	./build/test_bwt_san

# Data-race check of the page cache (concurrent put/get/erase test)
tsan: | build
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread $(FAST_SRC) tests/test_fast.cpp -o build/test_fast_tsan
	./build/test_fast_tsan

# Windows x64 executables via MinGW-w64 (apt install g++-mingw-w64-x86-64-posix); static, no DLLs needed
windows: | build
	x86_64-w64-mingw32-g++ -std=c++17 -O3 -Wall -Wextra -static -pthread $(FAST_SRC) src/amdcf_main.cpp -o build/amdcf.exe
	x86_64-w64-mingw32-g++ -std=c++17 -O3 -Wall -Wextra -static -pthread $(CM_SRC) src/main.cpp -o build/amdc1.exe
	x86_64-w64-mingw32-g++ -std=c++17 -O3 -Wall -Wextra -static -pthread $(BWT_SRC) src/amdcb_main.cpp -o build/amdcb.exe
	x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -static -pthread $(FAST_SRC) tests/test_fast.cpp -o build/test_fast.exe

clean:
	rm -rf build

.PHONY: all test crosscheck-fast sanitize tsan bench-fast windows clean
