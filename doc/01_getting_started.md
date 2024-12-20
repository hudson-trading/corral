## Your first async function

As a refresher, C++20 introduced support for _coroutines_: functions
whose execution can be suspended and resumed at various points, which
are marked by `co_await` or `co_yield` expressions in the body of the
function.  "Stack frames" for such functions are in fact allocated on
the heap, in order to store the function's parameters and local
variables between each suspension and its corresponding resumption.

Coroutines are a quite general language feature which can be used in
many different ways, some of which have nothing to do with concurrent
programming. This documentation will therefore generally speak of
**async functions** rather than coroutines. An async function is a
coroutine that is defined to return `corral::Task<T>`; `T` represents
the result type of the function. Inside an async function, you can use
a `co_await` expression to invoke another async function (or other
awaitable operation, described later) and wait for it to complete; and
you must use `co_return`, rather than plain old `return`, to specify
the `T` result.

(You can also call normal functions from an async function, using
normal function call syntax without `co_await`. We'll refer to normal
functions as "synchronous functions" when it's important to
distinguish them from async functions.)

Here's our first async function, not that complicated or interesting yet:

```cpp
boost::asio::io_service io_service;
corral::Task<void> async_hello() {
    std::cout << "getting ready..." << std::endl;
    co_await corral::sleepFor(io_service, 100ms);
    std::cout << "Hello, world!" << std::endl;
}
```

Things which can appear to the right of the `co_await` keyword are
called _awaitables_, and each one has an associated result type, which
is the type of the value produced by the `co_await` expression.  (The
result type of `corral::sleepFor()` is void.) Unsurprisingly, the task
handle `Task<T>` returned when you write a call to an async function
is itself awaitable, which allows you to call one async function from
another:

```cpp
corral::Task<void> wrap_hello() {
    std::cout << "Going to greet world" << std::endl;
    co_await async_hello();
    std::cout << "Greeted the world!" << std::endl;
}
```

OK, great... but how do we actually run these things?

If you look at the documentation for `corral::Task`, you'll see that
pretty much the only thing you can do with it is await it. And you can
only do that from another async function; if you try to write
`co_await` inside a normal, synchronous function, you'll get an
obscure compiler error, perhaps `unable to find the promise type for
this coroutine`. Since your C++ program starts out running a
synchronous function (`main()`), how do we get to the point where we
can run anything async?

The answer is `corral::run()`, an ordinary (synchronous) function
which accepts an event loop (such as the asio `io_service`) and an
awaitable (most likely the task handle that's returned when you call
an async function without `co_await`), and runs the event loop until
the task represented by the awaitable completes. So to run the
`wrap_hello()` function we defined above, we might write:

```cpp
int main() {
    corral::run(io_service, wrap_hello());
}
```

"Great," you might be saying. "What's the point?"

It's true that this example has not been particularly exciting so far;
we could've done the same thing with plain old `sleep()`. The actual
reason for using async functions is that they let you *do multiple
things at the same time*, with less overhead and less programming
complexity than many other ways of doing multiple things at the same time.
So let's try an example that runs two async functions in parallel:

```cpp
corral::Task<void> hello_twice() {
    std::cout << "Going to greet the world twice" << std::endl;
    co_await corral::allOf(async_hello(), async_hello());
}
int main() {
    corral::run(io_service, hello_twice());
}
```

This outputs:

    Going to greet the world twice
    getting ready...
    getting ready...
          <100ms pause>
    Hello, world!
    Hello, world!

While the first `co_await corral::sleepFor(io_service, 100ms)` was sleeping,
the second one was also able to run. Awaiting something suspends the
*current task* until the awaited operation completes, but other tasks can
still run during that time.

## What's in a task?

We used the word "task", and the type `corral::Task<T>`, several times
in the previous discussion. What do we mean by that, anyway?

A **task** is an independent thread of execution in corral's cooperative
multitasking environment. It might perform some meaningful operation, such
as "retrieve this web page from this server", by composing other tasks:
perhaps "look up the server's IP address", "connect to the server",
"send a request for the web page", and "process the response".
Its operations occur in a well-defined order, with the next not able to
begin until the previous one completes; but *other* (unrelated) tasks'
operations can be arbitrarily interleaved with them.

We say "task" instead of "thread" for consistency with other
frameworks, and because "thread" tends to suggest parallelism and its
associated data-race hazards. Only one corral task is actively
executing (as opposed to suspended at a `co_await` point) at any given
time. Using multiple tasks does not let you use more than 100% of one
CPU core (corral is not a tool for speeding up CPU-bound work) but it
does let your program do "multiple things at once" in the sense of having
multiple places for execution to resume when some action of interest
(receiving on a network socket, etc) completes.

Writing an async function is by far the easiest way to create a task
that does something novel, and the task handle object
`corral::Task<T>` wraps an invocation of specifically an async
function. Async functions are a bit heavyweight, though -- they
generally require a heap allocation for each invocation -- so most of
the primitive operations that corral provides are implemented using
custom awaitable objects rather than async functions. When writing
English, we will generally not insist upon a distinction between these
creatures; for example, we will say that `corral::allOf()` accepts a
series of "awaitables representing child tasks to run" even though some
of them might be implemented using async functions and some not. You
can imagine semantically that corral wraps the non-`Task` awaitables
in short async functions that do nothing but await them; while for
performance reasons this is not what actually occurs, it's a
distinction without a difference for most purposes.

## Combiners and nurseries

We've already seen the `allOf()` combiner in the example above; it
accepts a series of awaitables representing child tasks, runs all of them
concurrently, and returns a tuple of the results.

Another potentially useful combiner is `anyOf()`, which also runs multiple
tasks concurrently, but when the first one completes, it _cancels_ the rest.
In addition to the obvious use-case of running multiple tasks where you only
care about the first one that finishes, `anyOf()` can be used to implement
several different cancellation models:

* Attaching a timeout to a potentially long operation:
  ```cpp
  co_await corral::anyOf(somethingLong(),
                         corral::sleepFor(io_service, 3s));
  ```

* Attaching a `corral::Event` to make something externally cancellable:
  ```cpp
  corral::Event cancelEvent;
  co_await corral::anyOf(somethingLong(),
                         cancelEvent);
  // later, elsewhere:
  cancelEvent.set();
  ```

* Designing async functions (such as a TCP server's `accept()` loop) to run
  forever, and combining them with an awaitable that completes upon receipt
  of a signal (such as `boost::asio::signal_set`) in order to produce a
  clean shutdown on SIGTERM.

`anyOf()` returns a tuple of optional results; typically only one of the
optionals will be engaged, but it is possible for multiple tasks to produce
results close enough to simultaneously that the cancellation provoked by
the first result is too late to preempt the second.

Both `anyOf()` and `allOf()` come in two overloads: one that takes a
series of awaitables as arguments, and one that takes a range of
awaitables (such as a `std::vector`) as a single argument. The former
allows the types to vary but the quantity must be known at compile-time,
while the latter allows a runtime-variable quantity but requires all the
types to be the same.

All of the combiners described so far require the number of child tasks
to be known in advance, and take advantage of this knowledge to economize
on heap allocations. Corral also provides a more dynamic alternative,
which is called a *nursery* (mnemonic: a place for supervising your children).
The nursery starts by running a single task, which is specified inline
as the body of an async lambda, and this task can (directly or
indirectly) spawn additional tasks into the nursery by calling the
`start()` method of a `corral::Nursery&` to which it receives a reference.
Due to C++'s lack of support for writing destructors as async functions,
this requires some special syntax that uses a macro:

```cpp
corral::Task<void> foo() {
    CORRAL_WITH_NURSERY(n) {
        n.start(async_hello);
        n.start(wrap_hello);
        co_return corral::join;
    };
}
```

Of course, this example could more easily be written as:

```cpp
corral::Task<void> foo() {
    co_return co_await corral::allOf(async_hello(), wrap_hello());
}
```

But the nursery structure shines when you don't know how many tasks
you're going to run, such as when writing a server's `accept()` loop:

```cpp
corral::Task<void> serve(tcp::socket&);
corral::Task<void> acceptorLoop(tcp::acceptor& acc) {
    CORRAL_WITH_NURSERY(n) {
        while (true) {
            tcp::socket sock = co_await
                acc.async_accept(io_context, corral::asio_awaitable);
            n.start(serve, std::move(sock));
        }
    };
}
```

If the body of the nursery terminates (rather than looping forever),
it must return either `corral::join` or `corral::cancel`. `join` causes
the nursery to wait until all of its other tasks have completed normally,
while `cancel` will make it hurry them along by requesting their cancellation.

`Nursery::start()` accepts a callable which would produce an awaitable,
and arguments to it. Both are accepted by value, so any references passed
to `start()` will be copied into a temporary wrapper task, which ensures their
lifetime is extended until completion of the task. This allows async functions
to accept their arguments by reference. Passing arguments by reference
is possible, but has to be requested explicitly through wrapping
the reference into `std::ref` (or `std::cref` for a constant reference):

```cpp
corral::Task<void> foo(int&);
corral::Task<void> bar() {
    int x = 0;
    CORRAL_WITH_NURSERY(n) {
        for (int i = 0; i != 10; ++i) {
            n.start(foo, std::ref(x));
        }
        co_return corral::join;
    };
}
```

Generally only things defined _outside_ the nursery block can safely
be wrapped into `std::ref()` when passing to `Nursery::start()`.

Sometimes it may be necessary to submit a task into a nursery
and suspend until the task finishes initializing. While it is
possible to express this using existing synchronization primitives:

```cpp
corral::Event started;
CORRAL_WITH_NURSERY(n) {
    n.start([&]() -> corral::Task<> {
        // ...initialize...
        started.trigger();
        // ...work...
    });
    co_await started;
    // ...communicate with the task...
};
```
— the pattern is common enough that there is also a built-in option:
the callable passed to `Nursery::start()` may take a trailing
`corral::TaskStarted` parameter and invoke it after initialization.
When `Nursery::start()` receives such a callable, it returns an
awaitable such that `co_await nursery.start(...)` will both start
the task and wait for its initialization to complete. Using this
feature, the above example looks like:

```cpp
CORRAL_WITH_NURSERY(n) {
    co_await n.start([&](corral::TaskStarted<> started) -> corral::Task<> {
        // ...initialize...
        started();
        // ...work...
    });
    // ...communicate with the task...
};
```

The awaitable will also submit the task if it is destroyed before it is
awaited, so `n.start()` without `co_await` will work as it did without
the `TaskStarted` parameter. If you want to support using the same function
with both `n.start()` and with direct `co_await`, you can let it
accept a final argument of `corral::TaskStarted<> started = {}`, with
a default argument value. The default-constructed `TaskStarted` object
performs no operation when it is called.

When using this feature, the `TaskStarted` object is constructed internally
by the nursery, and will be passed to the function after all user-specified arguments.
This convention makes it difficult to combine use of `TaskStarted`
with default arguments.

A task submitted into a nursery may also communicate a value back
to its spawner, which becomes the result of the `co_await n.start()` expression:

```
CORRAL_WITH_NURSERY(n) {
    int v = co_await n.start([&](corral::TaskStarted<int> started) -> corral::Task {
        started(42);
        // ...further work...
    });
    assert(v == 42);
};
```

## The task tree

You might have noticed that we haven't said anything about spawning tasks
that outlive their parent task, or that don't have a parent task ("detached"),
both of which are common operations in other concurrency frameworks.
In fact, corral does not provide such functionality! It adheres to
_structured concurrency_ semantics which respect the following rule:

> A task can only run when it's being awaited by another task.

Of course, using the combiners and nurseries described above, a task
can await multiple other tasks simultaneously, and in fact this is the
primary mechanism for creating concurrency in corral applications. But
each task needs a parent somewhere, and the parent has to be waiting
(rather than doing other work), so that it is ready to promptly act on
any exceptional result of its children.

This structure where each task has one parent and each parent can have
multiple children logically produces a tree, which we call the "task
tree". The tree structure gives us several useful properties:

* Any unhandled exception in a child task can be reraised in its parent;
  if this process continues all the way up the tree, it will be raised
  out of `corral::run()`.

* If a task is cancelled (see below), the cancellation affects everything
  underneath it in the task tree; the cancellation request propagates
  down the tree, and corresponding outcomes of "I was cancelled rather
  than producing a value" propagate back up.

* Exceptions and cancellation can interact: if one child of a nursery/combiner
  throws an exception, the parent will cancel its other children in order to
  propagate the exception as quickly as possible. (Remember that the parent
  can't complete, including with an exception, while any of its children
  are still running; the parent must remain running to supervise the children
  for as long as any of the children are still alive.)

* C++ scopes and destructors work as expected:
  ```cpp
  corral::Task<void> foo() {
      std::ifstream ifs("/etc/passwd");
      co_await publishOnFacebook(ifs);

      // publishOnFacebook() is guaranteed to be done with ifs,
      // so it can be safely closed here
  }
  ```
  In this example, there is no way for `publishOnFacebook()`
  to continue running after it has returned (through spawning a detached
  task or something), so the caller can rely on the `ifs` destructor
  doing its job. Likewise, if the `foo()` task is being cancelled,
  it will first cancel the child `publishOnFacebook()`, and then
  destroy `ifs`.

There is a fantastic article by Nathaniel J. Smith,
["Notes on structured concurrency, or: Go statement considered harmful"](https://tinyurl.com/mvuecu3k),
which goes into more detail on the idea of structured concurrency;
corral users are _highly recommended_ to read it in order to
familiarize themselves thoroughly with the concept and its impliciations.

## Cancellation

As noted above, a task can be cancelled, most likely by the `anyOf()` combiner
when its sibling completes.

A cancellation request propagates recursively *down* the task tree:
when a task is to be cancelled, the cancellation request is forwarded
to whatever thing(s) it's awaiting (which might well be another task).
If it's not awaiting anything at the moment -- if it's actively
running, or scheduled on the executor to run imminently -- then the
cancellation request will be held until it awaits something and then
forwarded to that thing.

The actual cancellation, in the sense of interrupting an operation in
progress, is implemented by the primitive awaitables at the leaves of
the task tree. Each awaitable receives a cancellation request and
decides whether and when to confirm/"take" the cancellation (i.e.,
promise that the operation it represents is not going to complete
normally). Depending on the awaitable's implementation and the
constraints imposed by the operation it's modeling, it may take
cancellation immediately (synchronously) when requested, or after
a delay to perform some cleanup (asynchronously), or might not
support cancellation at all.

Most primitive operations are synchronously cancellable before they
start running, even if their awaitables were not written to support
cancellation; this is relevant if their parent task has a pending
cancellation, for example. Cancellation of an operation after it has
already started requires more cooperation from the awaitable (it might
need to coordinate with external entities, such as an operating system
to which it's submitted I/O requests) and is thus more likely to work
only asynchronously, or not at all.

When a primitive operation or other task is successfully cancelled,
the cancellation result propagates *up* the task tree. Its parent
effectively sees a third type of task outcome, besides the familiar "completed
by returning a value" and "completed by throwing an exception":
the new "completed by propagating cancellation". (We say "propagating"
cancellation because the cancellation request originally came from
outside the task that is completing with this status.)

* If the parent is an async function, it will respond to the
  cancellation of its child by destroying its local variables and
  parameters, and then propagating cancellation to its own parent.
  You can use a RAII guard such as `folly::ScopeGuard` to perform
  synchronous cleanup operations when a cancellation occurs. You _cannot_
  use a `catch` clause, not even `catch (...)`, because cancellation
  is not an exception.

* If the parent is a combiner, its behavior depends on the specific
  combiner. For example, `corral::anyOf()` will produce a result if
  _any_ of its child tasks produce a result, propagating cancellation
  only if all child tasks are cancelled. `corral::allOf()` is the
  reverse: it will produce a result only if _all_ of its child tasks
  produce results, which means it propagates cancellation if any of
  its child tasks were cancelled. (Both will propagate any exception
  raised by a child, even if other tasks were cancelled, because the
  exception preempts the question of how to construct the result value.)

By leveraging the task tree in this way, corral's cancellation system
makes it easy to cancel deeply nested task structures.  For example,
cancelling the `acceptorLoop()` task shown above will also cancel any
`serve()` tasks that it spawned.

In certain cases, resource cleanup needs to be asynchronous as well,
making it infeasible to use RAII. For such cases, corral provides an
equivalent of try/finally block which allows both clauses to be
asynchronous:

```cpp
struct AsyncFD {
    static corral::Awaitable<AsyncFD> auto open(std::string);
    corral::Awaitable<void> auto close();
};

corral::Task<void> workWithAsyncFD() {
    AsyncFD fd = co_await AsyncFD::open("...");
    co_await try_([&]() -> Task<void> {
        // do something with fd
    }).finally([&]() -> Task<void> {
        co_await fd.close();
    });
}
```

This way, regardless of whether the innermost task completes, exits
via exception, or is cancelled from the outside, `fd` will be closed
asynchronously, and the outer task will not get resumed until the close
completes.

Another syntax for the above is available, which creatively (ab)uses
C++ macros:

```cpp
corral::Task<void> workWithAsyncFD() {
    AsyncFD fd = co_await AsyncFD::open("...");
    CORRAL_TRY {
        // do something with fd
    }
    CORRAL_FINALLY {
        co_await fd.close();
    }; // <- the trailing semicolon is required here
}
```

Because merely entering or returning from an async function is not a
cancellation point _in itself_, the above code snippet can be
rewritten with more tasks without any changes to semantics
and without adding any additional cancellation points:

```cpp
corral::Task<AsyncFD> openAsyncFD() {
    co_return co_await AsyncFD::open("...");
}
corral::Task<void> consumeAsyncFD(AsyncFD fd) {
    CORRAL_TRY { /* ... */ }
    CORRAL_FINALLY { co_await fd.close(); };
}

corral::Task<void> workWithAsyncFD() {
    AsyncFD fd = co_await openAsyncFD();
    co_await consumeAsyncFD(std::move(fd));
}
```

If try/finally block was cancelled from the outside, the finally-clause
will not itself see the cancellation. If it's doing something that
might block indefinitely, it should impose an internal timeout to avoid
deadlocking the program.
