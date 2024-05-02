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

// An example asynchronous TCP server.
// Listens on a TCP ports, accepts connections,
// and echos all data back to clients.

#include <iostream>

#include "../corral/asio.h"
#include "../corral/corral.h"

using tcp = boost::asio::ip::tcp;

corral::Task<void> serve(tcp::socket sock) {
    try {
        std::array<char, 1024> data;
        while (true) {
            std::size_t length = co_await sock.async_read_some(
                    boost::asio::buffer(data), corral::asio_awaitable);
            co_await boost::asio::async_write(sock,
                                              boost::asio::buffer(data, length),
                                              corral::asio_awaitable);
        }
    } catch (boost::system::system_error& e) {
        if (e.code() == boost::asio::error::eof) {
            std::cerr << "connection closed\n";
        } else {
            std::cerr << "error in connection: " << e.what() << "\n";
        }
    }
}

corral::Task<void> echo_server(boost::asio::io_context& io_context,
                               unsigned short port) {
    tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port));
    CORRAL_WITH_NURSERY(nursery) {
        while (true) {
            tcp::socket sock = co_await acceptor.async_accept(
                    io_context, corral::asio_awaitable);
            std::cerr << "new connection accepted\n";
            nursery.start(serve, std::move(sock));
        }
    };
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("usage: asio_echo_server <port>");
        }
        unsigned short port = std::atoi(argv[1]);

        boost::asio::io_context io_context(1);
        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);

        std::cerr << "asio_echo_server: starting\n";
        corral::run(io_context,
                    corral::anyOf(echo_server(io_context, port),
                                  signals.async_wait(corral::asio_awaitable)));
        std::cerr << "asio_echo_server: exiting\n";
    } catch (std::exception& e) {
        std::cerr << "asio_echo_server: " << e.what() << "\n";
        return 1;
    }
}
