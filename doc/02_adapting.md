## Custom awaitables

In order for the library to be of any use, you need to be able to
actually wait for things to happen. Chances are, the codebase with
which you want to use corral already has some asynchronous I/O
operations, perhaps expressed as a function accepting a continuation
callback, or a signal/slot mechanism. It would be nice if a corral
task could just `co_await` on them.

To accomplish this for a particular operation, you need to define a
type that manages the operation and conforms to the corral _awaitable_
interface, a superset of the C++20 awaitable concept.

A C++20 awaitable is a type which either exposes the following methods,
or has an `operator co_await()` that returns such a type. Consult your
favorite [C++ reference](https://en.cppreference.com/w/cpp/language/coroutines)
for more details; this is just an overview.

* `bool await_ready() const noexcept`
  : Check whether the operation has already completed. This is called
  before `await_suspend()` in order to see whether it's possible to
  skip the somewhat expensive coroutine frame operations that surround
  a call to `await_suspend()`. If `await_ready()` returns true, then
  `await_suspend()` will not be called and execution proceeds directly
  to `await_resume()`. It is safe to simply return false from
  `await_ready()` unconditionally.

* `void await_suspend(std::coroutine_handle<> handle)`
  : Start the operation, and arrange for `handle.resume()` to be called
  when it completes. (Some alternative return types are also available to
  request immediate resumption of the `handle` or immediate resumption of
  some other coroutine handle.)

* `T await_resume()`
  : Retrieve the result of the operation, by returning a value or
  raising an exception. The return type of this method determines the
  result type of the `co_await` expression.

Corral extends this interface to support cancellation with three more
methods:

* `bool await_early_cancel() noexcept`
  : Called to request cancellation of the operation before it has
  started, i.e., before `await_suspend()` has been called. May return
  true to take the cancellation immediately, or false to request that
  the operation should start as usual and (if possible) complete via
  cancellation soon thereafter.
  **If an awaitable does not implement this method, it is treated as
  though it provided an implementation that returns true**;
  that is, synchronous early cancel is supported for all awaitables by default
  unless overridden.

* `bool await_cancel(std::coroutine_handle<> handle) noexcept`
  : Called to request cancellation of the operation while it is running
  (`await_suspend()` has called but the handle passed to
  `await_suspend()` has not been resumed yet). If the cancellation can
  be confirmed immediately, this method should return true; otherwise,
  it should return false and arrange for `handle.resume()` to be
  called once the cancellation has been confirmed.  The handle is the
  same one that was passed to `await_suspend()`.

* `bool await_must_resume() const noexcept`
  : Called when the operation completes (the parent task's handle is resumed)
  after a cancellation, whether early or regular, that did not complete
  synchronously. Since coroutine resumption takes no arguments,
  `await_must_resume()` is necessary to distinguish between the two
  reasons why it might have occurred in this case: either the cancellation
  was finally confirmed, or the operation completed before the cancellation
  could take effect. Returning true means the operation completed normally
  and its parent should proceed with `await_resume()`; returning false
  means the operation terminated by cancellation and its parent must _not_
  call `await_resume()`.

There are also two optional methods which are unrelated to cancellation:

* `void await_set_executor(corral::Executor* ex) noexcept`
  : Gain access to the corral `Executor`. This will be called
  before `await_suspend()` if it is defined. The executor can be passed
  to other awaitables you're wrapping (it's mostly needed by `Task`), and
  its `capture()` method can be used as an escape hatch to run a task step
  synchronously even if you're in the middle of another task step.

* `void await_introspect(auto& c) noexcept`
  : Call methods on `c` to describe the current awaitable and its children,
  for use in printing the task tree. The actual type of the `auto` is
  `corral::detail::TaskTreeCollector` as of this writing, but you
  should use `auto` to refer to it. `await_introspect()` should do
  `c.node("description", this)` to describe this awaitable, followed by
  `c.child(someChild)` for each child awaitable.

Some additional notes about the cancellation system:

* Both `await_early_cancel()` and `await_cancel()` may be defined to return
  `std::true_type` to indicate statically that that type of cancellation
  _always_ succeeds synchronously. If `await_cancel()` returns `std::true_type`,
  and `await_early_cancel()` either returns `std::true_type` or is not
  defined at all, then no definition of `await_must_resume()` is required,
  because it will never be invoked. If you do provide a definition of
  `await_must_resume()`, such as because it makes writing a generic
  awaitable wrapper more convenient, then it must return `std::false_type`
  to indicate that you know the awaiting/parent task will never resume normally
  after a cancellation.

* `await_must_resume()` may return `std::false_type` to indicate that it is
  never necessary to retrieve the awaitable's result after a cancellation.
  An awaitable may choose to declare this even if the result is technically
  available, as long as it has no side effects if unobserved.

* If `await_early_cancel()` returns false, no additional call
  to `await_cancel()` will be made; the awaitable is expected to remember
  that `await_early_cancel()` was called, and try to cancel the operation
  (if applicable) immediately after it starts.

* When deciding what to do with an awaitable that hasn't started yet,
  `await_early_cancel()` and `await_ready()` may be called in either order.
  If `await_early_cancel()` returns false and `await_ready()` returns true,
  then `await_must_resume()` will be called to decide between proceed
  with normal execution (`await_resume()`) or propagating cancellation.

* It is permissible for `await_cancel()` to resume its argument handle
  synchronously (before it returns). In that case, it must return false;
  returning true would constitute a double resumption which is undefined.
  `await_must_resume()` _will_ be called in this case, and has the
  opportunity to cause execution of the parent task to continue normally.

See the comments in `corral/concepts.h` for more details on corral's
extensions to the awaitable interface, including the names of several C++20
concepts that can be used to describe awaitables which implement them in
various ways.

As noted above, an awaitable object can define `await_*()` methods
(such classes are called _awaiters_ in the C++ standard), or return
an awaiter from `operator co_await()` (which can be a member or a free
function).

### The awaiter state machine

There is an implicit state machine that controls the order in which
corral can invoke the above methods, which is described in this section.
If you `#define CORRAL_AWAITABLE_STATE_DEBUG` before including any
corral headers, the state machine will become explicit and runtime checks
will be enabled that confirm the rules in this section are followed.
(This is not recommended except for debugging purposes, as it is rather slow.)

      .___________________________________.
      |     EC                            |
      |             .-------------> ReadyImmediately ------.
      |            /       __________^             \       |
      |           / R     / R                   !EC \__    |
      |          /       /    !EC                      |   |
      |   .------\- Initial ------> InitialCxlPend -.  |   |
      |   |       \    |                 |          |  |   |
      |   | EC     \   | !R              | !R     R |  |   |
      v   v    EC   \  v      !EC        v          |  |   |
    Cancelled <---- NotReady ------> CancelPending  |  |   |
      ^   ^            |                 |     |    |  |   |
      |   |    suspend |         suspend |     |    |  |   |
      |   |            v     !C          v     | R  |  |   |
      |   '------- Running -------> Cancelling |    |  |   |
      |     C          |                 |     |    |  |   |
      |         resume |          resume |     |   /   |   |
      |                v      MR         v     v  v    |   |
      |              Ready <------ ReadyAfterCancel <--'   |
      |                |             /                     |
      `________________|____________/                      |
                       |     !MR                           |
                       |                                   |
                       | await_resume()                    |
                       v            await_resume()         |
                     Done <--------------------------------'

    Legend:    EC: await_early_cancel() returned true (or wasn't defined)
              !EC: await_early_cancel() returned false
                R: await_ready() returned true
               !R: await_ready() returned false
                C: await_cancel() returned true
               !C: await_cancel() returned false
               MR: await_must_resume() returned true
              !MR: await_must_resume() returned false
          suspend: await_suspend() called
           resume: handle.resume() called

An awaiter may only be destroyed in the initial state (`Initial`, you
haven't yet touched it) or one of the terminal states (`Cancelled`, `Done`).
In the typical case where the awaiter is stored as a temporary in the
awaiting task's coroutine frame, the destruction would occur once
`await_resume()` finishes, or when the coroutine frame is destroyed
to propagate a cancellation.

* `Initial`: A new awaiter starts here. We don't know if it's ready yet, and
  need to check that before calling `await_suspend()`. We can also
  early-cancel before checking readiness.
  * Can call `await_early_cancel()`, transitioning to `Cancelled` if it
    returns true or `InitialCxlPend` otherwise.
  * Can call `await_ready()`, transitioning to `ReadyImmediately` if it
    returns true or `NotReady` otherwise.

* `NotReady`: We know the awaiter is not ready, so we're allowed to
  suspend it now. We can also check the readiness redundantly, or early-cancel.
  * Can call `await_early_cancel()`, transitioning to `Cancelled` if it
    returns true or `CancelPending` otherwise.
  * Can call `await_ready()`, transitioning to `ReadyImmediately` if it
    returns true or staying in `NotReady` otherwise.
  * Otherwise, call `await_suspend()`. Transition to `Running` before making the
    call. If it returns true, treat that as equivalent to a resumption of
    the handle we passed to `await_suspend()`.

* `ReadyImmediately`: The awaiter was ready without a suspension.
  Since it didn't suspend yet, we can still try an early cancel, or
  else we can proceed to consume the result.
  * Can call `await_ready()` redundantly but it is expected to continue
    returning true.
  * Can call `await_early_cancel()`, transitioning to `Cancelled`
    if it returns true or `ReadyAfterCancel` otherwise.
  * Otherwise, call `await_resume()` and transition to `Done`.

* `InitialCxlPend`: We did an early-cancel and it did not succeed
  immediately. We still don't know if the awaiter is ready, so need to
  check that.
  * Call `await_ready()`, transitioning to `ReadyAfterCancel` if it
    returns true or `CancelPending` otherwise.

* `CancelPending`: We did an early-cancel and it did not succeed
  immediately. We also know the awaiter is not ready, so we can suspend.
  And it's fine to check readiness redundantly as well.
  * Can call `await_ready()`, transitioning to `ReadyAfterCancel` if it
    returns true or staying in `CancelPending` otherwise.
  * Otherwise, call `await_suspend()`. Transition to `Cancelling`
    before making the call. If it returns true, treat that as
    equivalent to a resumption of the handle we passed to
    `await_suspend()`.

* `Running`: We have started the operation by calling `await_suspend()`,
  it hasn't completed yet and there's no cancel requested (yet).
  * Can call `await_cancel()`, transitioning to `Cancelled` if it returns
    true or `Cancelling` otherwise.
  * Otherwise just wait for the handle to be resumed, and transition to
    `Ready` when it is.

* `Cancelling`: The operation is running and also a cancellation was
  requested (either early or regular).
  * Nothing we can do but wait for the handle to be resumed, and transition
    to `ReadyAfterCancel` when it is.

* `ReadyAfterCancel`: The operation has completed after a previous cancellation
  was requested and did not succeed immediately. We need to determine whether
  the operation was ultimately cancelled or produced a result.
  * Call `await_must_resume()`. Transition to `Ready` if it returns true,
    `Cancelled` otherwise.

* `Ready`: The operation has completed, not via cancellation, and we
  need to consume its result.
  * Call `await_resume()` and transition to `Done`.

* `Done`: The operation has completed and we retrieved the result.
  * Destroy the awaiter and continue with normal execution of the parent.

* `Cancelled`: The operation completed via cancellation.
  * Destroy the awaiter and propagate cancellation into the parent.

Again, these states are shown for illustration purposes only;
they are not stored anywhere (except when using the state-debugging
`#define` explained above), and the awaiter is not required
to keep track of them.

### Example: wrapping a callback-based operation

Let us suppose we have an asynchronous function for reading
from a socket, which invokes a provided callback when the operation
completes (sometimes called "continuation-passing style" or CPS):

```cpp
void async_read(int fd, void* buf, size_t len,
                std::function<void(ssize_t)> cb);
```

We can wrap it into a corral awaitable like this:

```cpp
class AsyncRead {
    int fd_;       // A copy of the arguments
    void* buf_;
    size_t len_;
    ssize_t result_;

  public:
    AsyncRead(int fd, void* buf, size_t len):
        fd_(fd), buf_(buf), len_(len) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) {
        async_read(fd_, buf_, len_, [handle, this](ssize_t result) {
             result_ = result;
             handle.resume();
        });
    }

    ssize_t await_resume() const { return result_; }
};
```

Now we can use it, for example, like this:

```cpp
corral::Task<void> my_read_all(int fd) {
    std::array<char, 1024> buf;
    while (true) {
        ssize_t len = co_await AsyncRead(fd, buf.data(), buf.size());

        if (len > 0) { /* use buf[0..len] somehow */ }
        else if (len == 0) co_return; // EOF
        else throw std::runtime_error(strerror(errno));
    }
}
```

Note that this awaitable is not cancellable, because there is no mechanism
provided to cancel the underlying operation we're trying to wrap. We can't
implement `await_cancel()` unless we have a way to prevent the operation
from completing. (In this case, that should prevent the read from
occurring either, because we don't want to lose data.) A task that is
cancelled while it's waiting for the read to complete will continue waiting,
resume, and only propagate the cancellation on its next `co_await`.

Now let's imagine a better world, where the operation we're trying to
wrap *does* provide a cancellation capability, by having
`async_read()` return a handle that can be passed to an
`async_cancel_read()` function. We can use it to make our awaitable
cancellable like so:

```cpp
void* async_read(int fd, void* buf, size_t len,
                 std::function<void(ssize_t)> cb);
void async_cancel_read(void*);

class AsyncRead {
    // in addition to the above:

    void* read_handle_;

  public:
    void await_suspend(std::coroutine_handle<> handle) {
        read_handle_ = async_read(fd_, buf_, len_, [handle, this](ssize_t result) {
             result_ = result;
             handle.resume();
        });
    }

    auto await_cancel(std::coroutine_handle<> handle) noexcept {
        async_cancel_read(read_handle_);
        return std::true_type{};
    }
};
```

In this first cancellation example, we assumed that
`async_cancel_read()` would _immediately guarantee_ that the
`async_read()` completion callback won't be invoked. We might expect
behavior like that for a readiness-based event loop, such as one based
on `select` or `epoll`. Completion-based systems, such as `io_uring`
or Windows IOCP, typically expose an _asynchronous_ cancellation
operation instead. Let's suppose we get an
`async_try_cancel_read(void*)` function, which doesn't guarantee
anything immediately, but causes the callback to later be invoked with
`-ECANCELED` if the cancellation request ultimately succeeded in
preventing the read from completing. We would adapt that to corral
like so:

```cpp
void* async_read(int fd, void* buf, size_t len,
                 std::function<void(ssize_t)> cb);
void async_try_cancel_read(void*);

class AsyncRead {
    // in addition to the above:

    void* read_handle_;

  public:
    void await_suspend(std::coroutine_handle<> handle) {
        read_handle_ = async_read(fd_, buf_, len_, [handle, this](ssize_t result) {
             result_ = result;
             handle.resume();
        });
    }

    bool await_cancel(std::coroutine_handle<> handle) noexcept {
        async_try_cancel_read(read_handle_);
        return false;
    }

    bool await_must_resume() const noexcept {
        return result_ != -ECANCELED;
    }
};
```

### Example: wrapping a signal/slot interaction

Qt uses another approach for asynchronous operations: an abstraction
known as "signals and slots". For example, a `QTcpSocket` has a
`size_t bytesAvailable()` accessor, a `ssize_t read(void*, size_t)`
method which might block but only if the passed size exceeds
`bytesAvailable()`, and a signal `readyRead()` which is emitted when
`bytesAvailable()` changes.

While we could do something like the above to wrap socket reads
into an awaitable, for this example we'll generalize things a little bit
and produce a mechanism that can suspend a task until an arbitrary
Qt signal arrives.

(Certain details in the example below have been omitted for simplicity;
`examples/qt_echo_server.cc` has the full working version.)

```cpp
template<class Obj>
class QtSignalAwaitable {
    Obj* obj_;
    void (Obj::*signal_)();
    QMetaObject::Connection conn_;

  public:
    QtSignalAwaitable(Obj* obj, void (Obj::*signal)()):
        obj_(obj), signal_(signal) {}

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        conn_ = QObject::connect(obj_, signal_, [this, h] {
            QObject::disconnect(conn_); // only resume the waiting task once
            h.resume();
        });
    }
    void await_resume() { /*no value*/ }

    auto await_cancel(std::coroutine_handle<> h) {
        QObject::disconnect(conn_);
        return std::true_type{};
    }
};
```

Now we can use it like this:

```cpp
corral::Task<void> my_read_all(QTcpSocket& sock) {
    std::array<char, 1024> buf;
    while (true) {
        size_t avail;
        while ((avail = sock.bytesAvailable()) == 0)
            co_await QtSignalAwaitable(&sock, &QTcpSocket::readyRead);

        ssize_t len = sock.read(buf.data(), min(buf.size(), avail));

        if (len > 0) { /* use buf[0..len] somehow */ }
        else if (len == 0) co_return; // EOF
        else throw std::runtime_error(sock.errorString().toStdString());
    }
}
```

Note that since signals are naturally edge-triggered, we cannot unconditionally
wait on `readyRead`, because it might not be emitted if the socket is _already_
ready to read.

With this machinery, for example, we can run a task that is cancellable
by a GUI button press:

```cpp
co_await corral::anyOf(
    longRunningTask(),
    QtSignalAwaitable(cancelButton, &QPushButton::clicked));
```


### Movability of awaitables

Awaitables need to be movable in order to be passed to
`anyOf()` or `allOf()` combiners. However, most awaitables
cannot actually be moved once they're awaited, because
they need to be called back at a well-defined address once
the operation completes (both examples above demonstrate
this need: they capture `this` in lambdas they pass further down).

This contradiction can be resolved by splitting the awaitable
into two objects:

* an _operation state_ object, modelled by C++ _awaiter_ class
  (that which defines the``await_*()`` methods); it logically
  represents the state of an in-progress operation, and can be immovable;

* a higher-level _operation description_ object that returns the awaiter
  from its `operator co_await()`; it logically describes
  which operation is to be done, but doesn't need to be involved
  in the process of performing it, and can therefore be movable.

Each Corral combiner or adaptor that wraps awaitables (`anyOf()`,
`noncancellable()`, etc) both expects and conforms to this model:

* It requires its arguments to be movable, which follows naturally
  if the arguments are operation descriptions (implementing `operator
  co_await()` rather than the `await_*()` methods)

* It returns a movable operation description object that, when awaited,
  will construct an awaiter that comprises awaiters constructed
  from its arguments; due to C++17 mandatory copy elision,
  this can all be done in-place, allowing all the awaiters
  to be immovable.

Adapting `AsyncRead` from the above example would therefore look
like this:

```cpp
class AsyncRead {
    // In addition to the above:
    AsyncRead(AsyncRead&&) = delete;
};

class AsyncReadBuilder {
    int fd_;
    void* buf_;
    size_t len_;

  public:
    AsyncReadBuilder(int fd, void* buf, size_t len):
        fd_(fd), buf_(buf), len_(len) {}

    AsyncRead operator co_await() && {
        return AsyncRead(fd_, buf_, len_);
    }
};


corral::Awaitable<ssize_t> auto asyncRead(int fd, void* buf, size_t len) {
    return AsyncReadBuilder(fd, buf, len);
}
```

While this approach allows passing `asyncRead()` into `anyOf()`
while keeping the `AsyncRead` class immovable, it requires a decent
amount of typing. In the common case where the description class
merely saves arguments for the constructor of the operation state
and forwards them in its `operator co_await()`, you can use
the utility function `corral::makeAwaitable()` to automatically
generate the operation description type:

```cpp
class AsyncRead { /* same as above */ };

corral::Awaitable<ssize_t> auto asyncRead(int fd, void* buf, size_t len) {
    return corral::makeAwaitable<AsyncRead>(fd, buf, len);
}
```

## Adapting an event loop

Since async functions only run when awaited, you need a call to
`corral::run()` to get the async portion of your program off the
ground.  In addition to starting the top-level async function, `run()`
needs to run an event loop that will
serve whatever needs you have for I/O and timeouts. Since corral
strives to be flexible with respect to the specific event loop
implementation you choose, it can accept any type of event loop to
run, but it needs a bit of help defining some basic operations: how to
start and stop it, for example. You can provide this help by
specializing the `corral::EventLoopTraits` class for your specific
event loop type.

For example, if we're running under Qt, whose `QApplication` exposes
`exec()` and `exit()` methods, we can write:

```cpp
namespace corral {
template<>
struct EventLoopTraits<QApplication> {
    static void run(QApplication& app) { app.exec(); }
    static void stop(QApplication& app) { app.exit(); }

    // Only used for asserting against nested run() on the same event
    // loop, which is not supported. It's OK to provide a no-op definition
    // if you promise not to try to do nested run()s.
    static bool isRunning(QApplication& app) noexcept { return false; }

    // Return a value identifying the event loop. This is used to tell whether
    // two executors are running in the same environment or whether one
    // is nested in the other; the latter would occur if you call a
    // synchronous function that secretly uses corral::run() (with its
    // own separate event loop) internally. You can use the address
    // of the event loop object as its ID unless you have a situation where the
    // same underlying event loop can be accessed via multiple objects.
    static EventLoopID eventLoopID(QApplication& app) { return EventLoopID(&app); }
};
}
```

Now we can run async tasks and use Qt event loop to deliver events
to them:

```cpp
corral::Task<int> asyncMain() {
    // spawn more tasks
    // wait on Qt signals
    // do awesome things
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    return corral::run(app, asyncMain());
}
```

In addition to the `EventLoopTraits`, you will want to define some awaitables
that wrap your event loop's operations, such as performing I/O or waiting
for a timeout or signal. Different event loops are implemented differently
enough in these respects that we can only refer you to your event loop's
documentation; we may have more examples available in the future.
