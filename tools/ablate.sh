#!/bin/sh
# Ablation study: total level-1 (full model) archive bytes over a directory with each input disabled.
# usage: tools/ablate.sh DIR     (ablated builds are NOT format-compatible; for measurement only)
set -e
cd "$(dirname "$0")/.."
DIR=${1:?usage: ablate.sh DIR}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
total() { t=0; for f in "$DIR"/*; do s=$("$1" compress -l 1 -j 1 "$f" - 2>/dev/null | wc -c); t=$((t+s)); done; echo $t; }
build() { g++ -std=c++17 -O3 -pthread $1 src/cm.cpp src/transforms.cpp src/container.cpp src/main.cpp -o "$T/$2"; }
build "" full; base=$(total "$T/full")
printf "%-28s %10s %8s\n" variant bytes delta
printf "%-28s %10d %8s\n" "full model" "$base" "-"
row() { build "$1" v; t=$(total "$T/v"); printf "%-28s %10d %+7.2f%%\n" "$2" "$t" "$(echo "scale=4; ($t-$base)*100/$base" | bc)"; }
# input lanes of the full profile (see src/cm.cpp): 0-14 stretch(P) of models 0-14, 15-16 match inputs, 17-28 bit-history inputs of models 3-14
names="order0 order1 order2 order3 order4 order5 order6 order8 sparse-c1c3 ctx9(word/stride) ctx10 ctx11 trigram/stride3 column-above/neighbours skip3/c1c2x match5 match12 hist-o3 hist-o4 hist-o5 hist-o6 hist-o8 hist-sparse hist-ctx9 hist-ctx10 hist-ctx11 hist-ctx12 hist-ctx13 hist-ctx14"
i=0; for n in $names; do row "-DAMDC_ABLATE_MASK=$((1<<i))" "- $n"; i=$((i+1)); done
# note: the history lanes are zeroed in the mixer only; their tables still update, so this measures the inputs, not the memory
