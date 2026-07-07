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

#include <boost/asio/ssl/impl/src.hpp>
#include <boost/beast/src.hpp>
