// This file is part of corral, a lightweight C++20 coroutine library.
//
// Copyright (c) 2024 Hudson River Trading LLC <opensource@hudson-trading.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
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

#include <fstream>
#include <iostream>

#include <boost/beast.hpp>

#include "../corral/asio.h"
#include "../corral/corral.h"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace posix = asio::posix;
namespace beast = boost::beast;
namespace http = beast::http;

/// An example function which downloads a file over HTTP using multiple
/// concurrent conections.
corral::Task<void> http_download(
        asio::io_service& io_service,
        std::string url,
        std::ofstream& out,
        std::variant<int /*concurrency*/,
                     std::pair<size_t /*ofs*/, size_t /*size*/>>
                concurrencyOrRange) {
    size_t ofs = 0;
    ssize_t len = 0;

    static constexpr const std::string_view scheme = "http://";
    if (!url.starts_with(scheme)) {
        throw std::runtime_error("only http:// is supported");
    }
    size_t slash = url.find('/', scheme.size());
    std::string host = url.substr(7, slash - scheme.size());
    std::string path = url.substr(slash);

    // Resolve and connect
    tcp::resolver resolver(io_service);
    auto endpoints = co_await resolver.async_resolve(
            tcp::resolver::query{host, "http"}, corral::asio_awaitable);
    tcp::socket sock(io_service);
    co_await asio::async_connect(sock, endpoints, corral::asio_awaitable);

    // Make and send HTTP request
    http::request<http::empty_body> req{http::verb::get, path, 11};
    req.set(http::field::host, host);
    if (concurrencyOrRange.index() == 1) {
        // We're a dependent task and was passed a byte range to download;
        // set Range header accordingly.
        std::tie(ofs, len) = std::get<1>(concurrencyOrRange);
        req.set(http::field::range, "bytes=" + std::to_string(ofs) + "-" +
                                            std::to_string(ofs + len - 1));
    }
    co_await http::async_write(sock, req, corral::asio_awaitable);

    // Prepare response parsers
    beast::flat_buffer httpbuf;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
    auto& body = parser.get().body();

    std::array<char, 65536> bodybuf;
    body.data = bodybuf.data();
    body.size = bodybuf.size();

    CORRAL_WITH_NURSERY(nursery) {
        if (concurrencyOrRange.index() == 0) {
            // Read the header, figure out content length, partition,
            // and spawn child tasks to read body.
            co_await http::async_read_header(sock, httpbuf, parser,
                                             corral::asio_awaitable);
            size_t totalSize = *parser.content_length();
            size_t chunkSize = totalSize / std::get<0>(concurrencyOrRange) + 1;

            for (size_t bound = chunkSize; bound < totalSize;
                 bound += chunkSize) {
                nursery.start(http_download(
                        io_service, url, out,
                        std::make_pair(bound, std::min(chunkSize,
                                                       totalSize - bound))));
                ofs = bound;
            }

            // Read the first partition ourselves.
            ofs = 0;
            len = chunkSize;
        }

        // Now read the body.
        while (len > 0) {
            auto [ec, rd] = co_await http::async_read_some(
                    sock, httpbuf, parser, corral::asio_nothrow_awaitable);
            if (ec && ec != http::error::need_buffer) {
                throw beast::system_error(ec);
            }
            ssize_t sz = bodybuf.size() - body.size;
            std::cerr << "now at " << ofs << ", got " << sz << " bytes\n";
            out.seekp(ofs);
            out.write(bodybuf.data(), sz);
            ofs += sz, len -= sz;

            body.data = bodybuf.data();
            body.size = bodybuf.size();
        }

        co_return corral::join;
    };
}

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            throw std::runtime_error("usage: asio_http_downloader <url> "
                                     "<outfile> [<concurrency>]");
        }
        std::string url = argv[1];
        std::ofstream out(argv[2]);
        int concurrency = (argc > 3) ? std::stoi(argv[3]) : 1;

        asio::io_context io_context(1);
        corral::run(io_context,
                    http_download(io_context, url, out, concurrency));
    } catch (std::exception& e) {
        std::cerr << "asio_echo_server: " << e.what() << "\n";
        return 1;
    }
}
