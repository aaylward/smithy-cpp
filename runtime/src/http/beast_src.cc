// Deliberately NOT included here: <boost/beast/src.hpp> and
// <boost/asio/impl/src.hpp>. The BCR boost.beast module ships its own
// compiled implementation since 1.90 (boost.beast.src.cpp — at 1.87 this TU
// had to provide it), and boost.asio has compiled its implementation itself
// since 1.87.0.bcr.1; providing either again is an ODR violation (both
// caught by ASan in CI, one per bump).
//
// asio's *SSL* implementation IS included here: the BCR module only compiles
// it behind its `ssl` build flag, which every consumer would have to set on
// the command line. Compiling it in this TU (against the direct BoringSSL
// dependency) keeps //runtime:http_beast self-contained; it may exist only
// once per binary, which holds because //runtime:http_beast is the sole
// Beast/asio consumer in this repo.

// The module's header glob exports .ipp files but not the src.hpp umbrella,
// so this replicates <boost/asio/ssl/impl/src.hpp> (asio 1.87) inline.
#define BOOST_ASIO_SOURCE

#include <boost/asio/detail/config.hpp>

// clang-format off: upstream's order is load-bearing — context.ipp reaches
// socket_types.hpp (winsock/windows.h) first, which the later .ipp files'
// Windows static-mutex code needs already included.
#include <boost/asio/ssl/impl/context.ipp>
#include <boost/asio/ssl/impl/error.ipp>
#include <boost/asio/ssl/detail/impl/engine.ipp>
#include <boost/asio/ssl/detail/impl/openssl_init.ipp>
#include <boost/asio/ssl/impl/host_name_verification.ipp>
// clang-format on
