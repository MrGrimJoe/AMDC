CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread
CM_SRC   := src/cm.cpp src/transforms.cpp src/container.cpp
FAST_SRC := src/fast.cpp

all: build/amdc1 build/amdcf build/test_amdc1 build/test_fast

build:
	mkdir -p build

build/amdc1: $(CM_SRC) src/main.cpp src/amdc1.hpp src/transforms.hpp | build
	$(CXX) $(CXXFLAGS) $(CM_SRC) src/main.cpp -o $@

build/amdcf: $(FAST_SRC) src/amdcf_main.cpp src/fast.hpp src/pagecache.hpp | build
	$(CXX) $(CXXFLAGS) $(FAST_SRC) src/amdcf_main.cpp -o $@

build/test_amdc1: $(CM_SRC) tests/test_amdc1.cpp src/amdc1.hpp src/transforms.hpp | build
	$(CXX) $(CXXFLAGS) $(CM_SRC) tests/test_amdc1.cpp -o $@

build/test_fast: $(FAST_SRC) tests/test_fast.cpp src/fast.hpp src/pagecache.hpp | build
	$(CXX) $(CXXFLAGS) $(FAST_SRC) tests/test_fast.cpp -o $@

test: all
	./build/test_amdc1
	./build/test_fast
	sh tests/crosscheck.sh

# Memory-safety run of the fast codec (decoder fuzz included)
sanitize: | build
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread $(FAST_SRC) tests/test_fast.cpp -o build/test_fast_san
	./build/test_fast_san

# Windows x64 executables via MinGW-w64 (apt install g++-mingw-w64-x86-64-posix); static, no DLLs needed
windows: | build
	x86_64-w64-mingw32-g++ -std=c++17 -O3 -Wall -Wextra -static -pthread $(FAST_SRC) src/amdcf_main.cpp -o build/amdcf.exe
	x86_64-w64-mingw32-g++ -std=c++17 -O3 -Wall -Wextra -static -pthread $(CM_SRC) src/main.cpp -o build/amdc1.exe
	x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -static -pthread $(FAST_SRC) tests/test_fast.cpp -o build/test_fast.exe

clean:
	rm -rf build

.PHONY: all test sanitize windows clean
