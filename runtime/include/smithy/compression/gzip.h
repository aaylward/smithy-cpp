#ifndef SMITHY_COMPRESSION_GZIP_H_
#define SMITHY_COMPRESSION_GZIP_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "smithy/core/outcome.h"

namespace smithy {

// Gzip-compresses data (@requestCompression request bodies). Inputs of any
// size: bytes reach zlib's 32-bit counters in bounded slices, never through
// a truncating cast (issue #109).
Outcome<std::string> GzipCompress(std::string_view data);

// Decompresses a gzip stream, refusing outputs larger than max_output
// (decompression-bomb guard for server-side request bodies). The whole
// input must be the one stream — trailing bytes are an error.
Outcome<std::string> GzipDecompress(std::string_view data,
                                    std::size_t max_output = std::size_t{64} * 1024 * 1024);

namespace internal {

// The feed-size-parameterized cores: the public functions pass a bound that
// provably fits zlib's 32-bit avail_in; tests pass tiny bounds so the
// input re-feed loop is exercised without gigabyte fixtures.
Outcome<std::string> GzipCompressChunked(std::string_view data, std::size_t max_feed);
Outcome<std::string> GzipDecompressChunked(std::string_view data, std::size_t max_output,
                                           std::size_t max_feed);

}  // namespace internal

}  // namespace smithy

#endif  // SMITHY_COMPRESSION_GZIP_H_
