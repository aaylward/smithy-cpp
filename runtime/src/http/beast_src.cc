// The BCR boost.beast module defines BOOST_BEAST_SEPARATE_COMPILATION for its
// consumers but ships no compiled sources, so exactly one translation unit in
// the final link must provide Beast's implementation. This is that TU; it may
// exist only once per binary, which holds because //runtime:http_beast is the
// sole Beast consumer in this repo.
//
// Deliberately NOT included here: <boost/asio/impl/src.hpp>. The BCR
// boost.asio module (1.87.0.bcr.1+) compiles asio's implementation itself;
// providing it again is an ODR violation (caught by ASan in CI).
//
// asio's *SSL* implementation IS included here: the BCR module only compiles
// it behind its `ssl` build flag, which every consumer would have to set on
// the command line. Compiling it in this TU (against the direct BoringSSL
// dependency) keeps //runtime:http_beast self-contained — same
// exactly-one-TU rule as Beast's implementation above.

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

#include <boost/beast/src.hpp>
