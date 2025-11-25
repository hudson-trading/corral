// This file is part of corral, a lightweight C++20 coroutine library.
//
// Copyright (c) 2024-2025 Hudson River Trading LLC
// <opensource@hudson-trading.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// SPDX-License-Identifier: MIT

#define CATCH_CONFIG_MAIN
#include "config.h"

#if __cpp_exceptions

#include <chrono>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "../corral/asio.h"

#ifdef ASIO_STANDALONE
#include <asio.hpp>

#include "../corral/asio-standalone.h"
#define CORRAL_ASIOS                                                           \
    corral::detail::BoostAsioImpl, corral::detail::StandaloneAsioImpl
#else
#define CORRAL_ASIOS corral::detail::BoostAsioImpl
#endif

#if defined(CORRAL_HAVE_OPENSSL)
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#endif

#include "../corral/ThreadPool.h"
#include "../corral/corral.h"
#include "helpers.h"

#define CORRAL_BOOST_ASIO_TEST_CASE(...)                                       \
    CORRAL_TEST_CASE_IMPL(boost::asio::io_context, io, __VA_ARGS__)


#define CORRAL_MULTI_ASIO_TEST_CASE(name, tags)                                \
    CORRAL_MULTI_ASIO_TEST_CASE_IMPL_1(__COUNTER__, name, tags)
#define CORRAL_MULTI_ASIO_TEST_CASE_IMPL_1(counter, name, tags)                \
    CORRAL_MULTI_ASIO_TEST_CASE_IMPL_2(counter, name, tags)
#define CORRAL_MULTI_ASIO_TEST_CASE_IMPL_2(counter, name, tags)                \
    template <class Asio>                                                      \
    static ::corral::Task<void> test_body_##counter(                           \
            typename Asio::io_context&);                                       \
    CATCH_TEMPLATE_TEST_CASE(name, tags, CORRAL_ASIOS) {                       \
        typename TestType::io_context io;                                      \
        corral::run(io, test_body_##counter<TestType>(io));                    \
    }                                                                          \
    template <class Asio>                                                      \
    static ::corral::Task<void> test_body_##counter(                           \
            typename Asio::io_context& io)


namespace {

using Clock = std::chrono::high_resolution_clock;
using namespace std::chrono_literals;

using tcp = boost::asio::ip::tcp;

CORRAL_MULTI_ASIO_TEST_CASE("asio-smoke", "[asio]") {
    typename Asio::steady_timer t(io);
    t.expires_after(100ms);
    auto from = Clock::now();
    co_await t.async_wait(corral::asio_awaitable);
    CATCH_CHECK(Clock::now() - from >= 90ms);
}

CORRAL_MULTI_ASIO_TEST_CASE("asio-anyof", "[asio]") {
    typename Asio::steady_timer t1(io), t2(io);
    t1.expires_after(100ms);
    t2.expires_after(500ms);
    auto from = Clock::now();
    auto [s1, s2] =
            co_await corral::anyOf(t1.async_wait(corral::asio_awaitable),
                                   t2.async_wait(corral::asio_awaitable));

    auto d = Clock::now() - from;
    CATCH_CHECK(d >= 90ms);
    CATCH_CHECK(d <= 150ms);

    CATCH_CHECK(s1);
    CATCH_CHECK(!s2);
}

CORRAL_BOOST_ASIO_TEST_CASE("asio-socket-smoke", "[asio]") {
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 0));
    co_await corral::allOf(
            [&]() -> corral::Task<> {
                tcp::socket sock(io);
                co_await acceptor.async_accept(sock, corral::asio_awaitable);
                co_await boost::asio::async_write(
                        sock, boost::asio::buffer("hello, world"),
                        corral::asio_awaitable);
            },
            [&]() -> corral::Task<> {
                tcp::socket sock(io);
                co_await sock.async_connect(acceptor.local_endpoint(),
                                            corral::asio_awaitable);
                char buf[12];
                size_t n = co_await boost::asio::async_read(
                        sock, boost::asio::buffer(buf), corral::asio_awaitable);
                CATCH_REQUIRE(std::string(buf, n) == "hello, world");
            });
}

CORRAL_BOOST_ASIO_TEST_CASE("asio-thread-pool", "[asio]") {
    corral::ThreadPool tp(io, 2);

    CATCH_SECTION("smoke-void") {
        std::atomic<bool> called = false;
        co_await tp.run([&] { called.store(true); });
        CATCH_CHECK(called.load());
    }

    CATCH_SECTION("smoke-value") {
        auto tid = co_await tp.run([] { return std::this_thread::get_id(); });
        CATCH_CHECK(tid != std::this_thread::get_id());
    }

    CATCH_SECTION("reference") {
        int x = 0;
        int& y = co_await tp.run([&x]() -> int& { return x; });
        CATCH_CHECK(&x == &y);

        int&& ry = co_await tp.run([&x]() -> int&& { return std::move(x); });
        CATCH_CHECK(&x == &ry);
    }

    CATCH_SECTION("exception") {
        CATCH_CHECK_THROWS_WITH(
                co_await tp.run([] { throw std::runtime_error("boo!"); }),
                Catch::Equals("boo!"));
    }

    CATCH_SECTION("cancellation-confirmed") {
        std::atomic<bool> confirmed{false};
        auto body = [&]() -> corral::Task<> {
            co_await tp.run([&](corral::ThreadPool::CancelToken cancelled) {
                while (!cancelled) {}
                confirmed = true;
            });
            CATCH_CHECK(!"should never reach here");
        };
        co_await corral::anyOf(body, corral::sleepFor(io, 1ms));
        CATCH_CHECK(confirmed.load());
    }

    CATCH_SECTION("cancellation-unconfirmed") {
        std::atomic<bool> cancelled{false};
        auto body = [&]() -> corral::Task<int> {
            int ret = co_await tp.run([&](corral::ThreadPool::CancelToken) {
                while (!cancelled.load(std::memory_order_relaxed)) {}
                return 42;
            });
            // Cancel token was not consumed, so this line should get executed
            co_return ret;
        };

        auto [ret, _] = co_await corral::anyOf(body, [&]() -> corral::Task<> {
            co_await corral::sleepFor(io, 1ms);
            cancelled = true;
        });
        CATCH_CHECK(ret == 42);
    }

    CATCH_SECTION("early-cancellation") {
        std::atomic<bool> called{false};
        co_await anyOf(std::suspend_never{}, tp.run([&] { called = true; }));
        CATCH_CHECK(!called.load());
    }
}

// A thread pool with zero threads runs tasks inline.
CORRAL_BOOST_ASIO_TEST_CASE("asio-degenerate-thread-pool", "[asio]") {
    corral::ThreadPool tp(io, 0);

    CATCH_SECTION("smoke-void") {
        bool called = false;
        co_await tp.run([&] { called = true; });
        CATCH_CHECK(called);
    }

    CATCH_SECTION("smoke-value") {
        auto tid = co_await tp.run([] { return std::this_thread::get_id(); });
        CATCH_CHECK(tid == std::this_thread::get_id());
    }

    CATCH_SECTION("reference") {
        int x = 0;
        int& y = co_await tp.run([&x]() -> int& { return x; });
        CATCH_CHECK(&x == &y);

        int&& ry = co_await tp.run([&x]() -> int&& { return std::move(x); });
        CATCH_CHECK(&x == &ry);
    }

    CATCH_SECTION("exception") {
        CATCH_CHECK_THROWS_WITH(
                co_await tp.run([] { throw std::runtime_error("boo!"); }),
                Catch::Equals("boo!"));
    }

    CATCH_SECTION("early-cancellation") {
        std::atomic<bool> called{false};
        co_await anyOf(std::suspend_never{}, tp.run([&] { called = true; }));
        CATCH_CHECK(!called.load());
    }
}

CORRAL_BOOST_ASIO_TEST_CASE("asio-thread-pool-stress", "[asio]") {
    corral::ThreadPool tp(io, 8);
    auto work = [](int length) {
        static std::atomic<uint32_t> threadID{0};
        static thread_local std::mt19937 rng{++threadID};
        double cutoff = 1.0 / length;
        uint64_t c = 0;
        while (std::generate_canonical<double, 32>(rng) > cutoff) {
            ++c;
        }
        return c;
    };
    auto runStress = [&](int length, int count) -> corral::Task<uint64_t> {
        uint64_t ret = 0;
        corral::Semaphore sem{1000};
        CORRAL_WITH_NURSERY(n) {
            while (count--) {
                n.start([&sem, &ret, &work, &tp, length]() -> corral::Task<> {
                    auto lk = co_await sem.lock(); // limit concurrency
                    ret += co_await tp.run(work, length);
                });
            }
            co_return corral::join;
        };
        co_return ret;
    };

    CATCH_SECTION("short-tasks") {
        // These tasks run around 1us each
        co_await runStress(500, 100000);
    }
    CATCH_SECTION("long-tasks") {
        // These run around 100us each
        co_await runStress(50000, 1000);
    }
}

namespace beast = boost::beast;
namespace http = beast::http;

CORRAL_BOOST_ASIO_TEST_CASE("beast-http", "[asio]") {
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 0));
    co_await corral::allOf(
            [&]() -> corral::Task<> {
                tcp::socket sock(io);
                co_await acceptor.async_accept(sock, corral::asio_awaitable);

                beast::flat_buffer buffer;
                http::request<http::empty_body> req;
                co_await http::async_read(sock, buffer, req,
                                          corral::asio_awaitable);

                http::response<http::string_body> res;
                res.result(http::status::ok);
                res.set(http::field::content_type, "text/plain");
                res.keep_alive(false);
                res.body() = "hello, world";
                res.prepare_payload();
                co_await http::async_write(sock, res, corral::asio_awaitable);
            },

            [&]() -> corral::Task<> {
                tcp::socket sock(io);
                co_await sock.async_connect(acceptor.local_endpoint(),
                                            corral::asio_awaitable);

                http::request<http::empty_body> req(http::verb::get, "/", 11);
                req.set(http::field::host, "example.org");
                req.prepare_payload();
                co_await http::async_write(sock, req, corral::asio_awaitable);

                beast::flat_buffer buffer;
                http::response<http::dynamic_body> res;
                co_await http::async_read(sock, buffer, res,
                                          corral::asio_awaitable);

                CATCH_CHECK(res.result() == http::status::ok);
                std::string body = beast::buffers_to_string(res.body().data());
                CATCH_CHECK(body == "hello, world");
            });
}

#ifdef CORRAL_HAVE_OPENSSL
namespace ssl = boost::asio::ssl;

std::pair<std::string /*cert*/, std::string /*privateKey*/>
generateSelfSignedCert() {
    EVP_PKEY* pkey = EVP_PKEY_new();
    RSA* rsa = RSA_generate_key(2048, RSA_F4, nullptr, nullptr);
    EVP_PKEY_assign_RSA(pkey, rsa);

    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L);

    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (unsigned char*) "US",
                               -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                               (unsigned char*) "Boost.Beast", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (unsigned char*) "localhost", -1, -1, 0);

    X509_set_issuer_name(x509, name);

    X509_sign(x509, pkey, EVP_sha1());

    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, x509);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(bio, &bptr);

    std::string cert(bptr->data, bptr->length);
    BIO_free_all(bio);

    bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    BIO_get_mem_ptr(bio, &bptr);

    std::string key(bptr->data, bptr->length);
    BIO_free_all(bio);

    X509_free(x509);
    EVP_PKEY_free(pkey);

    return {cert, key};
}

CORRAL_BOOST_ASIO_TEST_CASE("beast-https", "[asio]") {
    ssl::context sslCtx(ssl::context::tlsv12);
    auto [cert, key] = generateSelfSignedCert();
    sslCtx.use_certificate_chain(boost::asio::buffer(cert));
    sslCtx.use_private_key(boost::asio::buffer(key),
                           ssl::context::file_format::pem);

    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 0));
    co_await corral::allOf(
            [&]() -> corral::Task<> {
                tcp::socket sock(io);
                co_await acceptor.async_accept(sock, corral::asio_awaitable);
                beast::ssl_stream<beast::tcp_stream> stream(std::move(sock),
                                                            sslCtx);
                co_await stream.async_handshake(ssl::stream_base::server,
                                                corral::asio_awaitable);

                beast::flat_buffer buffer;
                http::request<http::empty_body> req;
                co_await http::async_read(stream, buffer, req,
                                          corral::asio_awaitable);

                http::response<http::string_body> res;
                res.result(http::status::ok);
                res.set(http::field::content_type, "text/plain");
                res.keep_alive(false);
                res.body() = "hello, world";
                res.prepare_payload();
                co_await http::async_write(stream, res, corral::asio_awaitable);
            },

            [&]() -> corral::Task<> {
                beast::ssl_stream<beast::tcp_stream> stream(io, sslCtx);
                co_await beast::get_lowest_layer(stream).async_connect(
                        acceptor.local_endpoint(), corral::asio_awaitable);
                co_await stream.async_handshake(ssl::stream_base::client,
                                                corral::asio_awaitable);

                http::request<http::empty_body> req(http::verb::get, "/", 11);
                req.set(http::field::host, "example.org");
                req.prepare_payload();
                co_await http::async_write(stream, req, corral::asio_awaitable);

                beast::flat_buffer buffer;
                http::response<http::dynamic_body> res;
                co_await http::async_read(stream, buffer, res,
                                          corral::asio_awaitable);

                CATCH_CHECK(res.result() == http::status::ok);
                std::string body = beast::buffers_to_string(res.body().data());
                CATCH_CHECK(body == "hello, world");
            });
}
#endif

} // namespace

#endif // __cpp_exceptions
