#!/bin/sh
# Ablation study: total level-0 archive bytes over a directory with each component disabled.
# usage: tools/ablate.sh DIR     (ablated builds are NOT format-compatible; for measurement only)
set -e
cd "$(dirname "$0")/.."
DIR=${1:?usage: ablate.sh DIR}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
total() { t=0; for f in "$DIR"/*; do s=$("$1" compress -l 0 -j 1 "$f" - 2>/dev/null | wc -c); t=$((t+s)); done; echo $t; }
build() { g++ -std=c++17 -O3 -pthread $1 src/cm.cpp src/transforms.cpp src/container.cpp src/main.cpp -o "$T/$2"; }
build "" full; base=$(total "$T/full")
printf "%-28s %10s %8s\n" variant bytes delta
printf "%-28s %10d %8s\n" "full model" "$base" "-"
row() { build "$1" v; t=$(total "$T/v"); printf "%-28s %10d %+7.2f%%\n" "$2" "$t" "$(echo "scale=4; ($t-$base)*100/$base" | bc)"; }
names="order0 order1 order2 order3 order4 order6 stride/word-ctx stride2/sparse-ctx order8 sparse-c1c3 match-model"
i=0; for n in $names; do row "-DAMDC_ABLATE_MASK=$((1<<i))" "- $n"; i=$((i+1)); done
row "-DAMDC_NO_APM=1" "- APM/SSE stage"
row "-DAMDC_NO_MIX2=1" "- second mixer (by c1)"
