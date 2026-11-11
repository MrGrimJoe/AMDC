MinGW-w64 x64 static builds (no DLLs), cross-compiled on Linux from this source tree (amdc1 format v3).
These binaries have NOT been executed: the build environment had neither Windows nor Wine. The earlier Wine
test (1378/1378 checks) was of a previous build. Run test_fast.exe first on your machine; the same source
passes all tests on Linux (make test).

  amdcf.exe compress   -l 1 file file.amdf      fast decode (LZ4-compatible blocks)
  amdcf.exe decompress file.amdf file
  amdcf.exe bench file                           in-memory speed test
  amdcf.exe ramsim -p 4096 file1 file2 ...       how much RAM page compression would save, and page-in latency
  amdc1.exe compress [-l 0|1|2] [-m BITS] ...    maximum-ratio codec (slow, ~0.5 MB/s; -l 0 -m 20 uses ~63 MB)
  amdc1.exe decompress in out
Archives from amdc1 versions before this one (format v2) are not readable by this build.
