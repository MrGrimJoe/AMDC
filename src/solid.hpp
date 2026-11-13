// AMDC-S: optional solid multi-file archive on top of the AMDC-F block codec.
//
// Ordinary AMDC-F compresses every file (and every 256 KiB block) independently. AMDC-S additionally lets the LZ77
// window see data from OTHER files: files are split into groups of G, the pages of a group are interleaved into one
// "virtual stream" (page 0 of every file, then page 1 of every file, ...), and that stream is cut into independent
// stripes that are compressed with the unchanged AMDC-F block compressor (LZ4 block format, 64 KiB window).
// Stripes are independently decodable and parallel. The block codec itself is not modified.
//
// Archive layout (little endian), magic "AMDS", version 1:
//   header   : "AMDS", version=1, flags=0, varints G, U, SB, nfiles, meta_raw_len, meta_stored_len, meta_type
//   meta     : (LZ4 block if meta_type==1) per file: varint path_len, path bytes, varint size, u64 hash64(content)
//   stripe directory : per stripe, in group order: type byte (0 raw, 1 LZ4 block, 2 fill) + varint stored length
//   stripe data
//   trailer  : u64 hash64 of every preceding byte
// Files appear in the archive in the order they were given to pack(); consecutive runs of G files form a group.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "fast.hpp"

namespace amdc1 {
namespace solid {

using fast::Bytes;
constexpr size_t kPage = 4096;

struct SolidError : std::runtime_error {
    explicit SolidError(const std::string& m) : std::runtime_error(m) {}
};

struct File {
    std::string path;  // relative, '/' separated, validated by path_is_safe()
    Bytes data;
};

struct Options {
    unsigned group = 4;                 // files per group (1 = no cross-file sharing)
    unsigned unit_pages = 1;            // interleave unit in 4 KiB pages; 0 = whole files back to back (plain solid)
    size_t stripe_bytes = size_t(256) << 10;  // independent stripe size
    int level = 1;                      // AMDC-F block level (1 fast, 2 smaller)
    unsigned threads = 0;               // 0 = hardware concurrency; output is identical for any value
};

// A path is accepted only if it is relative, uses '/', has no empty/"."/".." components, no backslash, colon, NUL or
// control characters, and every component is <= 255 bytes.
bool path_is_safe(const std::string& p);

// Throws SolidError on unsafe or duplicate paths. validate=false is for tests that need to build hostile archives.
Bytes pack(const std::vector<File>& files, const Options& opt, bool validate = true);

// Rejects malformed archives (bad magic/version/limits, trailer or per-file hash mismatch, unsafe/duplicate/conflicting
// paths, size or ratio implausibility). `max_output` bounds the total decoded size.
std::vector<File> unpack(const uint8_t* in, size_t n, uint64_t max_output = uint64_t(1) << 32, unsigned threads = 0);

// Writes files below `dir` (created if missing). Never writes outside it: refuses symlinked or non-directory path
// components, refuses to overwrite existing files unless `force`, and re-validates every path.
void extract_to(const std::string& dir, const std::vector<File>& files, bool force = false);

}  // namespace solid
}  // namespace amdc1
