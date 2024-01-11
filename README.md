# Corral — lightweight structured concurrency for C++20

## Purpose

Corral is a C++ concurrency library that implements cooperative
single-threaded multitasking using C++20 coroutines. Its design is
based on our experience using coroutines to support asynchronous I/O in
real-world production code. Users familiar with the
[Trio](https://github.com/python-trio/trio) library for Python will
find a lot here that looks familiar. A few of corral's design goals are:

* ***[Structured concurrency](https://vorpus.org/blog/notes-on-structured-concurrency-or-go-statement-considered-harmful/)***
  baked in: tasks are organized into a tree of parent-child
  relationships, where the parent is responsible for waiting for its
  children to finish and propagates any exceptions that the children
  raise.  This allows certain crucial features like resource
  management, task cancellation, and error handling to Just Work™ the
  way people would expect them to.

* ***I/O and event loop agnostic***: like quite a few other
  companies with decades of history, we have our own homegrown
  implementations of asynchronous I/O and event loops. We wanted
  to be able to use coroutines with them, as well as pretty much
  any other existing solution for asynchronous I/O (such as Boost.Asio,
  libuv, or libevent).

* ***Bridging with callbacks***: the majority of existing code uses
  callbacks for asynchronous I/O; rewriting all of it from the ground
  up, while entertaining, tends not to be a realistic option.  We
  needed a way to have coroutine "pockets" in the middle of legacy
  code, being able call or be called from older code that's still
  using callbacks — so people could onboard gradually, one small piece
  at a time, getting benefit from these small pieces immediately.

Corral focuses on **single-threaded** applications because this results in
a simpler design, less overhead, and easier reasoning about
concurrency hazards. (In a single-threaded environment with
cooperative multitasking, you know that you have exclusive access to
all state in between `co_await` points.)  Multiple threads can each
run their own "corral universe", as long as tasks that belong to
different threads do not interact with each other.

## Motivating example

The code snippet below establishes a TCP connection to one of two
remote servers, whichever responds first, and returns the socket.

```cpp
using tcp = boost::asio::tcp;
boost::asio::io_service io_service;

corral::Task<tcp::socket> myConnect(tcp::endpoint main, tcp::endpoint backup) {
    tcp::socket mainSock(io_service), backupSock(io_service);

    auto [mainErr, backupErr, timeout] = co_await corral::anyOf(
        // Main connection attempt
        mainSock.async_connect(main, corral::asio_nothrow_awaitable),

        // Backup connection, with staggered startup
        [&]() -> corral::Task<boost::system::error_code> {
            co_await corral::sleepFor(io_service, 100ms);
            co_return co_await backupSock.async_connect(
                backup, corral::asio_nothrow_awaitable);
        },

        // Timeout on the whole thing
        corral::sleepFor(io_service, 3s));

    if (mainErr && !*mainErr) {
        co_return mainSock;
    } else if (backupErr && !*backupErr) {
        co_return backupSock;
    } else {
        throw std::runtime_error("both connections failed");
    }
}
```

## Prerequisites and installation

Corral is a header-only library, so you can just copy the `corral`
subdirectory into your project and start using it. It does not depend
on any external libraries on its own. If you want to build the tests,
you will need Catch2, and the examples require various other I/O libraries.

Obviously a recent C++ compiler is required. The library has been tested on gcc-11
and clang-15+, and was known to occasionally ICE gcc-10.2 back in the day.

To allow people to explore more easily and quickly get something running,
corral ships with support for Boost.Asio out of the box: pass
`corral::asio_awaitable` (or `corral::asio_nothrow_awaitable`) to any asio async
operation to make it return a corral-compatible awaitable. Other I/O frameworks
or event loops can also be adapted to corral relatively straightforwardly.
`examples/qt_echo_server.cc` shows bridging of corral and Qt.
