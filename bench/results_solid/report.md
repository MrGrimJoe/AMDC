
**Archive size as % of raw bytes (metadata included; independent = sum of per-file outputs)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 55.9 | 49.2 | 55.5 | 48.7 | 57.1 | 50.8 | 58.0 | 46.9 | 44.8 | 39.5 | 31.2 |
| libraries (50.3) | 52.2 | 45.9 | 52.2 | 45.9 | 54.3 | 48.3 | 54.2 | 44.4 | 41.8 | 37.5 | 30.0 |
| headers (42.9) | 37.1 | 31.4 | 30.3 | 23.3 | 34.9 | 28.5 | 30.0 | 22.1 | 20.2 | 17.7 | 12.9 |
| source_py (10.6) | 41.2 | 33.8 | 37.2 | 28.4 | 39.8 | 31.9 | 36.8 | 26.5 | 24.9 | 22.7 | 16.9 |
| source_versions (32.8) | 41.7 | 34.2 | 37.5 | 28.6 | 25.2 | 20.4 | 37.2 | 26.7 | 25.1 | 22.8 | 16.7 |
| text_docs (19.0) | 36.2 | 30.9 | 24.4 | 18.8 | 15.0 | 12.3 | 24.6 | 17.2 | 13.8 | 11.4 | 5.0 |
| mixed (50.3) | 54.5 | 50.0 | 53.1 | 48.3 | 53.9 | 49.3 | 53.8 | 47.4 | 44.6 | 42.3 | 38.0 |
| random_control (30.7) | 100.0 | 100.0 | 100.0 | 100.0 | 100.0 | 100.0 | 100.4 | 100.3 | 100.5 | 100.5 | 100.3 |

**Codec-only compression throughput, MB/s of raw data (in memory, no disk I/O)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 353 | 74 | 341 | 82 | 340 | 86 | 433 | 29 | 440 | 184 | 2 |
| libraries (50.3) | 395 | 82 | 372 | 89 | 370 | 82 | 458 | 29 | 415 | 194 | 2 |
| headers (42.9) | 381 | 86 | 553 | 118 | 463 | 114 | 519 | 37 | 439 | 291 | 2 |
| source_py (10.6) | 340 | 62 | 496 | 94 | 482 | 96 | 421 | 24 | 371 | 214 | 2 |
| source_versions (32.8) | 351 | 61 | 462 | 93 | 673 | 136 | 360 | 24 | 367 | 164 | 2 |
| text_docs (19.0) | 361 | 83 | 701 | 147 | 872 | 240 | 625 | 56 | 439 | 408 | 5 |
| mixed (50.3) | 393 | 84 | 534 | 96 | 435 | 100 | 614 | 33 | 469 | 218 | 3 |
| random_control (30.7) | 1877 | 75 | 993 | 102 | 894 | 113 | 1494 | 38 | 2041 | 1603 | 4 |

**Codec-only decompression throughput, MB/s of raw data (in memory, no disk I/O)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 1295 | 1378 | 1343 | 1731 | 1518 | 1582 | 1644 | 1869 | 1053 | 866 | 579 |
| libraries (50.3) | 1426 | 1378 | 1502 | 1650 | 1530 | 1508 | 1578 | 1682 | 871 | 858 | 512 |
| headers (42.9) | 1053 | 1036 | 1509 | 1644 | 1335 | 1373 | 1700 | 1861 | 958 | 1294 | 1055 |
| source_py (10.6) | 1095 | 1184 | 1432 | 1513 | 1401 | 1452 | 1662 | 1844 | 1234 | 1023 | 835 |
| source_versions (32.8) | 1168 | 1167 | 1220 | 1719 | 1677 | 1809 | 1614 | 1711 | 935 | 731 | 773 |
| text_docs (19.0) | 1033 | 967 | 1591 | 1772 | 1915 | 2003 | 2014 | 1778 | 1421 | 1502 | 2491 |
| mixed (50.3) | 1021 | 1298 | 1673 | 1662 | 1451 | 1415 | 1832 | 2005 | 1082 | 821 | 828 |
| random_control (30.7) | 3162 | 3230 | 1566 | 1981 | 1905 | 1715 | 3220 | 3113 | 6451 | 7033 | 5795 |

**End-to-end pack from directory: wall seconds (best of 3, includes reading files and writing the archive)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 0.31 | 0.85 | 0.37 | 0.85 | 0.33 | 0.82 | 0.21 | 2.09 | 0.23 | 0.37 | 27.51 |
| libraries (50.3) | 0.27 | 0.81 | 0.32 | 0.80 | 0.32 | 0.80 | 0.22 | 2.04 | 0.24 | 0.42 | 26.75 |
| headers (42.9) | 0.43 | 1.29 | 0.29 | 0.55 | 0.32 | 0.60 | 0.21 | 1.12 | 0.20 | 0.32 | 22.83 |
| source_py (10.6) | 0.35 | 0.51 | 0.07 | 0.15 | 0.08 | 0.17 | 0.06 | 0.46 | 0.07 | 0.11 | 6.31 |
| source_versions (32.8) | 1.07 | 1.45 | 0.20 | 0.46 | 0.16 | 0.37 | 0.15 | 1.34 | 0.17 | 0.22 | 19.91 |
| text_docs (19.0) | 0.79 | 1.01 | 0.11 | 0.22 | 0.11 | 0.17 | 0.10 | 0.39 | 0.12 | 0.10 | 3.77 |
| mixed (50.3) | 1.51 | 2.06 | 0.33 | 0.73 | 0.33 | 0.69 | 0.21 | 1.50 | 0.22 | 0.33 | 19.34 |
| random_control (30.7) | 0.21 | 0.63 | 0.15 | 0.45 | 0.15 | 0.40 | 0.11 | 0.84 | 0.10 | 0.12 | 8.43 |

**End-to-end unpack to directory: wall seconds (best of 2, includes writing every file)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 0.17 | 0.16 | 0.22 | 0.22 | 0.21 | 0.21 | 0.18 | 0.17 | 0.19 | 0.20 | 0.23 |
| libraries (50.3) | 0.17 | 0.13 | 0.16 | 0.18 | 0.19 | 0.15 | 0.21 | 0.21 | 0.21 | 0.24 | 0.28 |
| headers (42.9) | 0.26 | 0.50 | 1.29 | 1.91 | 1.84 | 1.59 | 1.92 | 1.76 | 2.23 | 1.80 | 1.80 |
| source_py (10.6) | 0.29 | 0.28 | 0.27 | 0.28 | 0.28 | 0.30 | 0.36 | 0.32 | 0.34 | 0.40 | 0.25 |
| source_versions (32.8) | 0.92 | 0.88 | 0.72 | 0.69 | 0.77 | 0.81 | 0.85 | 0.89 | 0.87 | 0.79 | 1.05 |
| text_docs (19.0) | 0.70 | 0.76 | 0.79 | 0.86 | 0.85 | 0.78 | 0.77 | 0.89 | 0.90 | 0.91 | 0.72 |
| mixed (50.3) | 0.85 | 0.99 | 1.40 | 1.27 | 1.28 | 1.57 | 1.42 | 1.46 | 1.39 | 1.12 | 1.63 |
| random_control (30.7) | 0.20 | 0.24 | 0.26 | 0.26 | 0.27 | 0.15 | 0.25 | 0.29 | 0.26 | 0.22 | 0.27 |

**Pack CPU seconds (user+sys, whole process tree)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 0.41 | 1.39 | 0.48 | 1.47 | 0.44 | 1.40 | 0.24 | 2.12 | 0.28 | 0.41 | 27.26 |
| libraries (50.3) | 0.36 | 1.34 | 0.41 | 1.36 | 0.42 | 1.35 | 0.26 | 2.06 | 0.28 | 0.46 | 26.58 |
| headers (42.9) | 0.43 | 1.29 | 0.34 | 0.88 | 0.39 | 0.94 | 0.25 | 1.15 | 0.24 | 0.36 | 22.67 |
| source_py (10.6) | 0.35 | 0.52 | 0.09 | 0.25 | 0.10 | 0.28 | 0.07 | 0.45 | 0.08 | 0.12 | 6.31 |
| source_versions (32.8) | 1.08 | 1.47 | 0.25 | 0.77 | 0.20 | 0.57 | 0.18 | 1.35 | 0.19 | 0.25 | 19.73 |
| text_docs (19.0) | 0.79 | 1.04 | 0.13 | 0.33 | 0.13 | 0.24 | 0.12 | 0.41 | 0.13 | 0.12 | 3.72 |
| mixed (50.3) | 1.54 | 2.32 | 0.40 | 1.20 | 0.39 | 1.11 | 0.26 | 1.55 | 0.26 | 0.38 | 19.28 |
| random_control (30.7) | 0.20 | 0.62 | 0.16 | 0.71 | 0.15 | 0.65 | 0.14 | 0.86 | 0.13 | 0.14 | 8.34 |

**Pack peak memory, MB (largest process in the tree)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 43 | 42 | 183 | 174 | 171 | 164 | 116 | 105 | 83 | 82 | 157 |
| libraries (50.3) | 30 | 26 | 163 | 159 | 175 | 169 | 112 | 103 | 82 | 81 | 157 |
| headers (42.9) | 15 | 15 | 128 | 122 | 138 | 138 | 81 | 74 | 66 | 66 | 144 |
| source_py (10.6) | 15 | 15 | 39 | 36 | 39 | 38 | 30 | 28 | 27 | 27 | 106 |
| source_versions (32.8) | 15 | 15 | 110 | 102 | 95 | 94 | 68 | 62 | 54 | 54 | 132 |
| text_docs (19.0) | 15 | 15 | 57 | 57 | 56 | 54 | 40 | 38 | 35 | 36 | 114 |
| mixed (50.3) | 30 | 31 | 168 | 163 | 189 | 180 | 113 | 107 | 85 | 85 | 162 |
| random_control (30.7) | 15 | 15 | 128 | 128 | 127 | 127 | 100 | 100 | 73 | 73 | 153 |

**Unpack peak memory, MB (largest process in the tree)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 32 | 33 | 85 | 82 | 89 | 86 | 136 | 131 | 17 | 18 | 24 |
| libraries (50.3) | 21 | 21 | 81 | 78 | 84 | 81 | 134 | 129 | 17 | 18 | 24 |
| headers (42.9) | 15 | 15 | 61 | 58 | 63 | 61 | 113 | 109 | 16 | 18 | 24 |
| source_py (10.6) | 15 | 15 | 21 | 20 | 21 | 21 | 37 | 36 | 16 | 18 | 24 |
| source_versions (32.8) | 15 | 15 | 50 | 48 | 47 | 45 | 89 | 86 | 16 | 18 | 24 |
| text_docs (19.0) | 15 | 15 | 30 | 29 | 28 | 28 | 55 | 54 | 16 | 18 | 24 |
| mixed (50.3) | 18 | 18 | 81 | 79 | 82 | 80 | 138 | 134 | 17 | 18 | 24 |
| random_control (30.7) | 15 | 15 | 68 | 68 | 68 | 68 | 100 | 100 | 16 | 18 | 24 |
