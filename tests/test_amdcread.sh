#!/bin/sh
# End-to-end check of amdcread against archives made by the real compressors. Usage: tests/test_amdcread.sh [BUILD_DIR]
set -e
B=${1:-build}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir "$T/src" "$T/scan" "$T/scan/deep"
i=0; while [ $i -lt 400 ]; do echo "line $i: the quick brown fox jumps over the lazy dog $((i*7919 % 1000))"; i=$((i+1)); done > "$T/src/a.txt"
head -c 30000 "$B/amdcf" > "$T/src/b.bin"
cp "$T/src/a.txt" "$T/in.txt"
fail=0
check() { if [ "$1" != "$2" ]; then echo "FAIL: $3"; fail=1; else echo "ok: $3"; fi; }

"$B/amdc1" compress -l 0 "$T/in.txt" "$T/scan/cm.weird"
"$B/amdcf" compress "$T/in.txt" "$T/scan/fast.noext"
"$B/amdcb" compress "$T/in.txt" "$T/scan/deep/bwt.AMDC"
"$B/amdcs" pack "$T/src" "$T/scan/solid.bin"
echo "not an archive, just text" > "$T/scan/plain.txt"

for f in cm.weird fast.noext deep/bwt.AMDC; do
  "$B/amdcread" cat "$T/scan/$f" > "$T/out.txt"
  cmp -s "$T/out.txt" "$T/in.txt" && r=same || r=differ
  check "$r" same "cat $f"
  "$B/amdcread" test "$T/scan/$f" | grep -q "OK" && r=ok || r=bad
  check "$r" ok "test $f"
done
"$B/amdcread" ls "$T/scan/solid.bin" | grep -q "a.txt" && r=ok || r=bad; check "$r" ok "ls solid"
"$B/amdcread" cat "$T/scan/solid.bin" a.txt | cmp -s - "$T/src/a.txt" && r=same || r=differ; check "$r" same "cat solid member"
"$B/amdcread" cat "$T/scan/solid.bin" b.bin | cmp -s - "$T/src/b.bin" && r=same || r=differ; check "$r" same "cat solid binary member"
n=$("$B/amdcread" scan "$T/scan" 2>/dev/null | wc -l); check "$n" 4 "scan finds exactly the four archives, whatever their names"
"$B/amdcread" info "$T/scan/plain.txt" | grep -q "not an AMDC-family" && r=ok || r=bad; check "$r" ok "info rejects plain text"
"$B/amdcread" info "$T/scan/fast.noext" | grep -q "decodes to" && r=ok || r=bad; check "$r" ok "info shows AMDF decoded size"
"$B/amdcread" "$T/scan/cm.weird" | grep -q "the quick brown fox" && r=ok || r=bad; check "$r" ok "bare FILE views it"

# damaged and truncated archives must be reported, never crash
head -c 100 "$T/scan/fast.noext" > "$T/trunc.amdf"
"$B/amdcread" test "$T/trunc.amdf" >/dev/null 2>&1 && r=accepted || r=rejected; check "$r" rejected "truncated AMDF rejected"
cp "$T/scan/deep/bwt.AMDC" "$T/dmg.bin"; printf 'XXXX' | dd of="$T/dmg.bin" bs=1 seek=40 conv=notrunc 2>/dev/null
"$B/amdcread" cat "$T/dmg.bin" > "$T/dmg.out" 2>/dev/null && cmp -s "$T/dmg.out" "$T/in.txt" && r=silently_ok || r=not_silently_ok
check "$r" not_silently_ok "damaged BWT archive is not returned as if intact"
# the output cap refuses a big decode
"$B/amdcread" -m 0 cat "$T/scan/fast.noext" >/dev/null 2>&1 && r=decoded || r=refused; check "$r" refused "-m 0 refuses to decode"
[ $fail = 0 ] && echo "amdcread: all checks passed" || { echo "amdcread: FAILED"; exit 1; }
