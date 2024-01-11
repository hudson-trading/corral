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

/// An example asynchronous TCP client. Connects to a remove server,
/// then forwards all data between the socket and the process's stdin/stdout.

#include <iostream>

#include "../corral/asio.h"
#include "../corral/corral.h"

using tcp = boost::asio::ip::tcp;
namespace posix = boost::asio::posix;


/// Forwards all data from `source` to `sink`.
corral::Task<void> forward(auto& source, auto& sink) {
    std::array<char, 1024> data;
    while (true) {
        auto [ec, length] = co_await source.async_read_some(
                boost::asio::buffer(data), corral::asio_nothrow_awaitable);
        if (!ec) {
            co_await boost::asio::async_write(sink,
                                              boost::asio::buffer(data, length),
                                              corral::asio_awaitable);
        } else if (ec == boost::asio::error::eof) {
            co_return;
        } else {
            throw boost::system::system_error(ec);
        }
    }
}

corral::Task<void> netcat(boost::asio::io_context& io_context,
                          std::string host,
                          std::string port) {
    tcp::socket sock(io_context);
    tcp::resolver resolver(io_context);

    // Resolve a DNS name and connect
    tcp::resolver::results_type endpoints = co_await resolver.async_resolve(
            {host, port}, corral::asio_awaitable);
    co_await boost::asio::async_connect(sock, endpoints,
                                        corral::asio_awaitable);

    // Wrap stdin/stdout in boost::asio streams
    posix::stream_descriptor stdin(io_context, ::dup(STDIN_FILENO));
    posix::stream_descriptor stdout(io_context, ::dup(STDOUT_FILENO));

    // Do the forwarding
    co_await corral::anyOf(forward(stdin, sock), forward(sock, stdout));
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::runtime_error("usage: asio_netcat <host> <port>");
        }

        boost::asio::io_context io_context(1);
        corral::run(io_context, netcat(io_context, argv[1], argv[2]));
    } catch (std::exception& e) {
        std::cerr << "asio_echo_server: " << e.what() << "\n";
        return 1;
    }
}
