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

#include <chrono>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#ifdef CORRAL_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#endif

#include "../corral/asio.h"
#include "../corral/corral.h"
#include "config.h"
#include "helpers.h"

#define CORRAL_TEST_CASE(...)                                                  \
    CORRAL_TEST_CASE_IMPL(boost::asio::io_service, io, __VA_ARGS__)

namespace {

using Clock = std::chrono::high_resolution_clock;
using namespace std::chrono_literals;

namespace asio = boost::asio;
using tcp = asio::ip::tcp;


CORRAL_TEST_CASE("asio-smoke", "[asio]") {
    asio::deadline_timer t(io);
    t.expires_from_now(boost::posix_time::millisec(100));
    auto from = Clock::now();
    co_await t.async_wait(corral::asio_awaitable);
    CATCH_CHECK(Clock::now() - from >= 90ms);
}

CORRAL_TEST_CASE("asio-anyof", "[asio]") {
    asio::deadline_timer t1(io), t2(io);
    t1.expires_from_now(boost::posix_time::millisec(100));
    t2.expires_from_now(boost::posix_time::millisec(500));
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

CORRAL_TEST_CASE("asio-socket-smoke", "[asio]") {
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 0));
    co_await corral::allOf(
            [&]() -> corral::Task<> {
                tcp::socket sock(io);
                co_await acceptor.async_accept(sock, corral::asio_awaitable);
                co_await asio::async_write(sock, asio::buffer("hello, world"),
                                           corral::asio_awaitable);
            },
            [&]() -> corral::Task<> {
                tcp::socket sock(io);
                co_await sock.async_connect(acceptor.local_endpoint(),
                                            corral::asio_awaitable);
                char buf[12];
                size_t n = co_await asio::async_read(sock, asio::buffer(buf),
                                                     corral::asio_awaitable);
                CATCH_REQUIRE(std::string(buf, n) == "hello, world");
            });
}


namespace beast = boost::beast;
namespace http = beast::http;

CORRAL_TEST_CASE("beast-http", "[asio]") {
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
namespace ssl = asio::ssl;

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

CORRAL_TEST_CASE("beast-https", "[asio]") {
    ssl::context sslCtx(ssl::context::tlsv12);
    auto [cert, key] = generateSelfSignedCert();
    sslCtx.use_certificate_chain(asio::buffer(cert));
    sslCtx.use_private_key(asio::buffer(key), ssl::context::file_format::pem);

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
