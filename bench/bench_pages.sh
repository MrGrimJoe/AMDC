#!/bin/sh
# usage: LZ4=/path/to/lz4/lib ZSTD=/path/to/zstd/lib bench/bench_pages.sh [bench args...]
set -e
FLAGS="-std=c++17 -O3 -march=native -pthread"
[ -n "$LZ4" ]  && FLAGS="$FLAGS -DHAVE_LZ4 -I$LZ4"
[ -n "$ZSTD" ] && FLAGS="$FLAGS -DHAVE_ZSTD -I$ZSTD"
LIBS=""
FLAGS="$FLAGS -DHAVE_ZLIB"; LIBS="$LIBS -lz"
[ -n "$LZ4" ]  && LIBS="$LIBS $LZ4/liblz4.a"
[ -n "$ZSTD" ] && LIBS="$LIBS $ZSTD/libzstd.a"
g++ $FLAGS src/fast.cpp bench/bench_pages.cpp $LIBS -o build/bench_pages
./build/bench_pages "$@"
