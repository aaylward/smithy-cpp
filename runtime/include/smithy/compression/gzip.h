#ifndef SMITHY_COMPRESSION_GZIP_H_
#define SMITHY_COMPRESSION_GZIP_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "smithy/core/outcome.h"

namespace smithy {

// Gzip-compresses data (@requestCompression request bodies).
Outcome<std::string> GzipCompress(std::string_view data);

// Decompresses a gzip stream, refusing outputs larger than max_output
// (decompression-bomb guard for server-side request bodies).
Outcome<std::string> GzipDecompress(std::string_view data,
                                    std::size_t max_output = std::size_t{64} * 1024 * 1024);

}  // namespace smithy

#endif  // SMITHY_COMPRESSION_GZIP_H_
