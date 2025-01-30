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

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <iostream>

#include "../corral/corral.h"

template <class Signal> class QtSignalAwaitable;

/// Wraps a Qt signal into a C++20 awaitable.
///
/// Awaiting on this will suspend the caller until the *next* signal is emitted;
/// any signals emitted before `co_await` would be discarded.
template <class Obj, class... Args>
class QtSignalAwaitable<void (Obj::*)(Args...)> {
    Obj* obj_;
    void (Obj::*signal_)(Args...);
    QMetaObject::Connection conn_;
    std::tuple<std::decay_t<Args>...> value_;

  public:
    QtSignalAwaitable(std::derived_from<Obj> auto* obj,
                      void (Obj::*signal)(Args...))
      : obj_(obj), signal_(signal) {
        // Typically (if feasible), a freshly constructed awaitable remains
        // in dormant state, and does not have any visible effect until
        // `await_suspend()` is called; so we merely store sender and signal.
    }

    bool await_ready() const noexcept {
        // As we always want the next signal, we are never ready.
        return false;
    }

    void await_suspend(corral::Handle h) {
        conn_ = QObject::connect(obj_, signal_, [this, h](Args... args) {
            // Stash signal arguments to later return from `await_resume()`.
            value_ = std::make_tuple(args...);
            QObject::disconnect(conn_); // so the lambda won't get called again
            h.resume();
        });
    }

    auto&& await_resume() && { return std::move(value_); }

    auto await_cancel(corral::Handle h) noexcept {
        QObject::disconnect(conn_); // Tell Qt not to call our lambda anymore
        return std::true_type{};    // confirm the cancellation
    }
};

template <class Obj, class Signal>
auto untilSignalled(Obj* obj, Signal signal) {
    return QtSignalAwaitable<Signal>(obj, signal);
}


namespace corral {
template <> struct EventLoopTraits<QCoreApplication> {
    static EventLoopID eventLoopID(QCoreApplication& app) {
        return EventLoopID(&app);
    }
    static void run(QCoreApplication& app) { app.exec(); }
    static void stop(QCoreApplication& app) { app.exit(); }
};
} // namespace corral

struct DeleteLater {
    void operator()(QObject* obj) const { obj->deleteLater(); }
};
template <class T> using QtUniquePtr = std::unique_ptr<T, DeleteLater>;


//
// Now let's use the above to implement a simple echo server
//

corral::Task<void> serve(QtUniquePtr<QTcpSocket> sock) {
    std::array<char, 4096> buf;
    while (true) {
        // Wait for data
        while (sock->state() == QTcpSocket::ConnectedState &&
               sock->bytesAvailable() == 0) {
            co_await corral::anyOf(
                    untilSignalled(sock.get(), &QTcpSocket::readyRead),
                    untilSignalled(sock.get(), &QTcpSocket::disconnected));
        }

        // Read data
        auto size = sock->read(buf.data(), buf.size());
        if (size <= 0) {
            std::cerr << "disconnected\n";
            co_return;
        }

        sock->write(buf.data(), size); // enqueue data

        // Wait for the data to be sent
        while (sock->state() == QTcpSocket::ConnectedState &&
               sock->bytesToWrite() > 0) {
            co_await corral::anyOf(
                    untilSignalled(sock.get(), &QTcpSocket::bytesWritten),
                    untilSignalled(sock.get(), &QTcpSocket::disconnected));
        }
    }
}

corral::Task<void> echoServer() {
    QtUniquePtr<QTcpServer> server(new QTcpServer);
    server->listen(QHostAddress::Any, 9987);
    CORRAL_WITH_NURSERY(n) {
        while (true) {
            while (!server->hasPendingConnections()) {
                co_await untilSignalled(
                        server.get(), &QTcpServer::pendingConnectionAvailable);
            }
            QtUniquePtr<QTcpSocket> sock(server->nextPendingConnection());
            std::cerr << "new connection\n";
            n.start(&serve, std::move(sock));
        }
    };
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    corral::run(app, echoServer());
    return 0;
}
