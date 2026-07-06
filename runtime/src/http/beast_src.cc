// The BCR boost.asio and boost.beast modules define
// BOOST_ASIO_SEPARATE_COMPILATION / BOOST_BEAST_SEPARATE_COMPILATION for
// their consumers but ship no compiled sources, so exactly one translation
// unit in the final link must provide both implementations. This is that TU;
// it may exist only once per binary, which holds because //runtime:http_beast
// is the sole Boost consumer in this repo.

// clang-format off
#include <boost/asio/impl/src.hpp>
#include <boost/beast/src.hpp>
// clang-format on
