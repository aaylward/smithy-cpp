#include "smithy/compression/gzip.h"

#include <zlib.h>

#include <array>

namespace smithy {

namespace {
constexpr int kGzipWindowBits = 15 + 16;  // 32KB window, gzip wrapper
constexpr std::size_t kChunk = std::size_t{16} * 1024;
}  // namespace

Outcome<std::string> GzipCompress(std::string_view data) {
  z_stream stream{};
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, kGzipWindowBits, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return Error::Serialization("gzip: deflateInit2 failed");
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());

  std::string out;
  std::array<char, kChunk> buffer{};
  int result = Z_OK;
  do {
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    result = deflate(&stream, Z_FINISH);
    if (result == Z_STREAM_ERROR) {
      deflateEnd(&stream);
      return Error::Serialization("gzip: deflate failed");
    }
    out.append(buffer.data(), buffer.size() - stream.avail_out);
  } while (result != Z_STREAM_END);
  deflateEnd(&stream);
  return out;
}

Outcome<std::string> GzipDecompress(std::string_view data, std::size_t max_output) {
  z_stream stream{};
  if (inflateInit2(&stream, kGzipWindowBits) != Z_OK) {
    return Error::Serialization("gzip: inflateInit2 failed");
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());

  std::string out;
  std::array<char, kChunk> buffer{};
  int result = Z_OK;
  do {
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    result = inflate(&stream, Z_NO_FLUSH);
    if (result != Z_OK && result != Z_STREAM_END) {
      inflateEnd(&stream);
      return Error::Serialization("gzip: malformed stream");
    }
    out.append(buffer.data(), buffer.size() - stream.avail_out);
    if (out.size() > max_output) {
      inflateEnd(&stream);
      return Error::Serialization("gzip: output exceeds limit");
    }
  } while (result != Z_STREAM_END);
  inflateEnd(&stream);
  if (stream.avail_in != 0) {
    return Error::Serialization("gzip: trailing garbage after stream");
  }
  return out;
}

}  // namespace smithy
