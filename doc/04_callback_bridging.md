## Interoperating with callback-oriented code

### Callback portals

As outlined [above](02_adapting.md), the preferred way to adapt
a callback-style asynchronous operation is to wrap it into an awaitable
object. However, if that's not feasible, corral provides a general-purpose
bridging mechanism that allows a task to wait for a callback to be invoked.

Let's suppose we want to write yet another echo server, but we're
constrained (for illustrative purposes) to use an existing interface
to listen for new connections. It has the signature:

    Listener doListen(int port, std::function<void(int /*newFD*/)> cb);

— with the semantics that, as long as the `Listener` remains in scope,
`cb` will be called for each new connection accepted.

With a little help from `corral::CBPortal`, we can arrange to consume
this stream of incoming connections in an async task:

    // defined elsewhere to handle a single connection
    corral::Task<void> serve(int fd);

    corral::Task<void> listen(int port) {
        CORRAL_WITH_NURSERY(n) {
            corral::CBPortal<int> cbp; // callback takes (int)
            std::optional<Listener> listener;

            // Set up the portal and wait for the first CB invocation
            int fd = co_await corral::untilCBCalled([&](auto cb) {
                listener = doListen(port, cb);
            }, cbp);

            while (true) {
                n.start(serve(fd));
                // Wait for another invocation
                fd = co_await cbp;
            }
        };
    }

`untilCBCalled()` accepts an "initiator function" (a lambda) and passes
it a "bridge callback" (a small trivially-copyable callable object,
suitable for efficient assignment to `std::function` or whatever other
callback type you might have available). The bridge callback has one
parameter for each template argument of the corresponding `CBPortal`,
and each invocation of the bridge callback will resume the task
that's waiting on the `CBPortal`; the bridge callback arguments
becoming the result of the `co_await` expression, wrapped in a `std::tuple`
if there are multiple of them. The initiator function's job is to
arrange for the bridge callback to be called. The unusual signature
of `untilCBCalled`, where you pass a callback that accepts a callback,
is necessary to support situations where the bridge callback is invoked
synchronously from the initiator function, as many callback-oriented
APIs like to do.

The bridge callback is passed by reference, and the referent remains
valid for as long as the `CBPortal` does. This makes it easy(ish) to
interface with C-style APIs that take arguments
`void (*cb)(void* /*cookie*/)`, `void* cookie`:

    void doSomething(void (*cb)(void*, int /*x*/, int /*y*/), void* cookie);

    corral::CBPortal<int, int> cbp;
    auto [x, y] = co_await corral::untilCBCalled([&]<class CB>(CB& cb) {
        doSomething(
            +[](void* arg, int x, int y) { (*(CB*)arg)(x, y); },
            &cb);
    }, cbp);

This is a lowish-level interface with some unavoidable sharp
edges. Some things to keep in mind:

* The bridge callback must be invoked once per `co_await` expression on
  the portal, plus once for the initial call to `untilCBCalled()`.
  Fewer invocations will cause the awaiting task to hang, while extra
  calls will likely result in a use-after-free error.

* Remember that the task that created the `CBPortal` can be cancelled at
  any time. It must arrange for this cancellation to prevent any further
  calls to the bridge callback; if this is impossible, it can refuse
  cancellation using the `corral::noncancellable()` wrapper.
  In the example above, we were careful to construct the `Listener`
  object after the `CBPortal` so that it would be destroyed beforehand.
  In other cases, it may be necessary to use a scope guard (`gsl::finally`
  or equivalent) to perform the cancellation.

* In many callback-based APIs that make multiple calls, the callback
  can be invoked multiple times sequentially without yielding control,
  so the task that's receiving the calls must be prepared to handle
  that.  If the task waits on something other than the portal while
  the portal is active, it might miss calls; the portal only
  internally stores one call's worth of arguments, and even those might
  be stale if not consumed immediately (if they contain references
  to data that doesn't outlive the callback invocation).

If an asynchronous operation accepts multiple callbacks (perhaps it
returns values and errors via different channels), you can pass multiple
`CBPortal`s to `untilCBCalled()`. The return type of `untilCBCalled()`
becomes `std::tuple<std::optional<>>` of the result types of each
individual portal (as with `anyOf()`), and the initiator function
receives one bridge callback per portal as separate arguments.
Subsequent callback invocations can be retrieved
through `co_await corral::anyOf(cbp1, cbp2, ...)`.

If you only need to wait for one call, you can skip the `CBPortal` and
just write:

    void doSomething(std::function<void(int /*x*/, int /*y*/)> resultCB);

    auto [x, y] = co_await corral::untilCBCalled(
            [&](std::function<void(int, int)> cb) {
                doSomething(cb);
            });

In this formulation, the signature(s) of the bridge callback(s) is
inferred from the parameter(s) of the initiator lambda, which
precludes using a generic lambda. If you need to capture a reference to
the callback, as was done in the "C-style" example above, you can write:

    void doSomething(void (*cb)(void*, int /*x*/, int /*y*/), void* cookie);

    using CBType = corral::CBPortal<int, int>::Callback;
    auto [x, y] = co_await corral::untilCBCalled([&](CBType& cb) {
        doSomething(
            +[](void* arg, int x, int y) { (*(CBType*)arg)(x, y); },
            &cb);
    });

Since the version of `untilCBCalled()` without portal arguments
constructs the portal internally, it can't rely on a RAII guard in
the enclosing scope to make sure the callback is disabled before further
invocation if the parent task is cancelled. Instead, you must pass a
second lambda (the cancellation function) if you want the wait to be
cancellable. See the documentation in `corral/CBPortal.h` for more details.

### Unsafe nurseries

Sometimes, it's necessary to bridge callback-style code with async
tasks in the opposite direction: writing an async function that
conforms to an existing callback-style interface.

Suppose we are given this interface for an abstract reader,
which we cannot change:

    class IReader {
      public:
        virtual void read(void* buf, size_t len,
                          std::function<void(ssize_t)> cb) = 0;
    };

— and we're implementing a reader with some nontrivial logic
we'd rather express as an async function.

If you can use a [live object](03_live_objects.md), this is pretty
easy; just start a task in your nursery that performs the operation
and then calls the callback:

    class MyReader : public IReader {
        corral::Nursery* nursery_ = nullptr;

        // complicated logic goes here:
        corral::Task<ssize_t> readImpl(void* buf, size_t len);

      public:
        corral::Task<void> run() { return corral::openNursery(nursery_); }

        void read(void* buf, size_t len, std::function<void(ssize_t)> cb) {
            nursery_->start([=]() -> corral::Task<> {
                try {
                    ssize_t result = co_await readImpl(buf, len);
                    cb(result);
                } catch (std::exception&) { cb(-1); }
            });
        }
    };

This will work fine if there is an appropriate "parent object" for
`MyReader` that can arrange to run `MyReader::run()` and thus manage
the nursery. But if you're just starting out with using coroutines
in an existing system, you probably don't have a clean path of async
tasks all the way up to `main()`. In this situation, corral offers an
escape from the structured concurrency rules: an "unsafe nursery".

    class MyReader : public IReader {
        corral::UnsafeNursery nursery_;

        // complicated logic goes here:
        corral::Task<ssize_t> readImpl(void* buf, size_t len);

      public:
        // Event loop must be specified explicitly because it can't
        // be inferred from the nursery parent, because there isn't one.
        // (The nursery doesn't directly use the event loop, but it needs
        // to be able to tell that its tasks are running "at the same time"
        // as other tasks that use this event loop.)
        MyReader(boost::asio::io_service& io) : nursery_(io) {}

        void read(void* buf, size_t len, std::function<void(ssize_t)> cb) {
            nursery_.start([=]() -> corral::Task<> {
                try {
                    ssize_t result = co_await readImpl(buf, len);
                    cb(result);
                } catch (std::exception&) { cb(-1); }
            });
        }
    };

An unsafe nursery acts as a supervisor for an arbitrary number of tasks,
just like a regular nursery does. And it has a public constructor and
destructor, so you can store it in an ordinary member variable. But
as the name suggests, it requires you to carefully observe additional
invariants:

* If a task in the unsafe nursery throws an exception, there is nowhere
  to deliver it, so it will crash the entire program using `std::terminate()`.
  (This is similar to what happens with unhandled exceptions in `main()` or
  `std::thread`.)

* When the unsafe nursery is destroyed, it should not have any tasks
  left in it. If there are some, corral will make one last-ditch
  effort to cancel them; but the `UnsafeNursery` destructor can't yield
  to the event loop, so this will only succeed if everything in the
  nursery is synchronously cancellable. If any tasks remain active after
  this attempt, you'll get an assertion failure; if you're running
  without assertions enabled, you'll see undefined behavior when the
  tasks eventually complete and try to send their results to a nursery
  that no longer exists.

Unsafe nurseries exist to violate the structured concurrency
semantics, in a somewhat controlled fashion, because in practice this
is sometimes necessary. Using them will make your program more
confusing and error-prone compared to propagating structured
concurrency semantics throughout. But engineering is all about
tradeoffs, and sometimes the tradeoffs for a particular application
favor the unsafe approach.

Each unsafe nursery comes fully loaded with its own `corral::Executor`
to manage running its child tasks, since it might be the only piece of
async infrastructure in the program. These exist alongside the single
executor created by `corral::run()`, if such a call is active. Tasks
in unsafe nurseries can interact freely with each other and with tasks
underneath a call to `corral::run()`, as long as all of them are using
the same event loop as identified by `EventLoopTraits<...>::eventLoopID()`.
The additional executor for each unsafe nursery does carry some cost
in the form of an additional heap allocation.

An unsafe nursery can be "adopted" into regular nursery semantics by
having another task do `co_await unsafeNursery.join();`. The task that
calls `join()` effectively becomes the nursery's new parent, waits for
all tasks in the nursery to exit and receives any exceptions they
raise, and ensures that no new tasks can be started in the nursery
after `join()` returns. If the joining task is cancelled, the tasks in
the nursery will be cancelled as well, and the joining task won't
resume until all the cancelled tasks have exited. If you can't arrange
to use `join()`, you will just need to be very careful about task
lifetimes.

Alternatively, one can `asyncClose()` a nursery. This function cancels
any tasks in the nursery (including those yet to be started) like
`cancel()` does, but also accepts a continuation callback, which will
be invoked when the tasks complete and the nursery is safe to destroy
(such destruction can happen from within the callback). This provides
an option to implement asynchronous destruction of an object
featuring UnsafeNursery if necessary:

    class MyReader: public IReader { // see above
        corral::UnsafeNursery nursery_;

      public:
        void asyncDestroy() {
            nursery_.asyncClose([this]() noexcept { delete this; });
        }
    };

`asyncDestroy()` may also be helpful if the object can be destroyed
from within the completion callback of a regular operation, like
`cb(result)` in the snippet above. Since the task calling the callback
is currently running, it cannot be cancelled synchronously, so an attempt
to delete `MyReader` from within such a callback would trigger the assert
mentioned above. To accommodate a pattern like this, it's possible to
heap-allocate the `UnsafeNursery` (in order to be able to decouple
its lifetime from the lifetime of `MyReader`) and call `asyncClose()`
from the destructor of `MyReader`:

    class MyReader: public IReader { // see above
        std::unique_ptr<corral::UnsafeNursery> nursery_;

      public:
        MyReader(boost::asio::io_service& io):
            nursery_(std::make_unique<corral::UnsafeNursery>(io));

        ~MyReader() {
            auto n = nursery_.release();
            n->asyncClose([n]() noexcept { delete n; })
        }
    };

Note that in the latter case any tasks still running might retain a pointer
to `MyReader` which is already destroyed; it's up to the user to guard
against any use-after-free errors.
