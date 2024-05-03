## Spawning tasks that outlive you

As described previously, Corral adheres to structured concurrency
principles, requiring each running task to be awaited by a parent task
that can accept its result (or exception) when the child terminates
and can cancel the child if the parent needs to exit.  Typically this
is done by letting the parent `co_await` the child, either directly or
through `anyOf()` or `allOf()` combiners, or having the parent open a
nursery using `CORRAL_WITH_NURSERY()` and start the child in that
nursery. In all these cases the child task is spawned by the same code
that waits for its result, but this is not required!

Sometimes a function really does need to spawn a task that will outlive it.
For example, if you need to provide a callback for an API that expects to
call a synchronous function, that function isn't going to be supervising any
tasks on its own, because synchronous functions can't yield to the event loop
and thus can't outlive an async task that probably does need to.

Luckily, there is an escape! The requirement is that _someone_
await/supervise each task, not that it be the same code that spawns
the task. If you obtain a nursery reference using
`CORRAL_WITH_NURSERY(n) { ... }`, you can pass it to another function,
store it in a variable, etc; and as long as the nursery object is
alive (i.e. until the block completes), any function which has access
to that reference may use it to spawn tasks into that nursery:

```cpp
void delayedPrint(corral::Nursery& n) {
    n.start([]() -> corral::Task<> {
        co_await corral::sleepFor(io_service, 1s);
        std::cout << "Hello, world!" << std::endl;
    });
}

corral::Task<void> func() {
    CORRAL_WITH_NURSERY(n) {
        delayedPrint(n);
        co_return corral::join;
    };
}
```

This allows bending the rules a little, but still in an organized way:

* There still _is_ a parent for each task, which is responsible for
  handling exceptions raised by its children, and can cancel them if
  necessary. In the above example, `func()` is the parent of the
  anonymous task that `delayedPrint()` spawns.

* The lifetime of all tasks in a nursery is still bounded by the lifetime
  of the nursery block; execution can't proceed past the semicolon after
  `CORRAL_WITH_NURSERY(n) { ... }` until there are no tasks that remain
  alive in the nursery.

* Passing nursery references explicitly allows us to see the "complexity
  degree" of a function simply by looking at its signature:

    * `void func()` — synchronous function, incapable of spawning async tasks

    * `corral::Task<void> func()` — async function, can spawn
      child tasks, but is required to join them before returning

    * `void func(corral::Nursery&)` (or `corral::Task<> func(corral::Nursery&)`) —
      can (and likely will) spawn tasks which will continue running
      after the function returns.

## Live objects

Frequently, it is useful to have a nursery that is effectively 'associated
with' a particular instance of a class, and thus can supervise tasks that
provide functionality for the object it's associated with. For example,
perhaps you have an object that represents an HTTP/2 connection, and you
want to allow different tasks to manage sending and receiving on the various
streams that are multiplexed over this connection. You might find it useful
to have a single task, owned by the connection, which reads messages from the
network socket and either responds to them directly (for session-level
traffic such as PING) or distributes them to the appropriate stream. Or if
you're implementing a class that talks to a set of remote servers, you might
want it to have an associated task that periodically pings the ones you think
are down, so you can notice promptly when they come back up.

At first glance, this looks challenging. The "support tasks" for an
object want to run in a nursery that is somehow owned or managed by
that object, but it can't simply be a member of the object because it
doesn't have a public constructor or destructor. (Nursery destruction
is logically asynchronous, because it must wait for the tasks in the
nursery to exit, so corral only permits you to create a nursery from
async code.)

You could require that the user of your object provide a nursery
reference when constructing it, but this winds up creating something
of a lifetime trap:

```cpp
CORRAL_WITH_NURSERY(n) {
    MyLiveClass obj(n);
    obj.doSomething();
    co_return corral::cancel;

    // at this point `obj` is destroyed (because it is local to this
    // block), but `n` is still alive, and background tasks of `obj`
    // spawned there are still running, potentially holding dangling
    // references to `obj`
};
```

Instead, we recommend an approach akin to two-phase initialization,
which we refer to as a "live object". The idea is that you define your
object to contain a nursery *pointer* as a member, and endow it with
an async method `run()` which opens the nursery, starts up the support
tasks, and stashes the pointer for other methods to use:

```cpp
class MyLiveClass {
    corral::Nursery* nursery_ = nullptr;
  public:
    corral::Task<void> run() {
        CORRAL_WITH_NURSERY(n) {
            auto guard = folly::makeGuard([this] { nursery_ = nullptr; });
            nursery_ = &n;
            CORRAL_SUSPEND_FOREVER();
        };
    }

    // Now `nursery_` can be used to spawn tasks
    corral::Task<void> asyncThing();
    void beginThing() {
        nursery_->start(asyncThing());
    }
};
```

This particular shape of `run()` can be simplified by using
`corral::openNursery()`, which does exactly the above:

```cpp
corral::Task<void> run() { return corral::openNursery(nursery_); }
```

(Note that this uses a trick we haven't seen yet: it's a synchronous
function which calls an async function and passes along the returned
task handle. This is a more efficient way to wrap an async function
than `co_return co_await corral::openNursery(nursery_);`, but both
work.)

Live objects typically do not do anything meaningful merely because
of having been constructed; one needs to `run()` them to make them
do things.

Live objects can naturally nest: all that's required is for the parent
object's `run()` to `run()` each of its child objects:

```cpp
class MyParentLiveClass {
    MyChildLiveClass1 child1_;
    MyChildLiveClass2 child2_;
  public:
    corral::Task<void> run() {
        co_await corral::allOf(child1_.run(),
                               child2_.run());
    }
};
```

Note that this parent class does not need a nursery for its own use,
but still needs `run()` to establish a parent task to supervise the `run()`
calls for each of its children. This results in an established hierarchy
of tasks that resembles the hierarchy of the live objects in the program.
