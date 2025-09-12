# API reference

## General notes on usage

All names are defined in `namespace corral`, except macros which are
prefixed with `CORRAL_`.

See `corral/config.h` for documentation on the semantics of various
`#define`s that you can provide to customize corral to fit your needs.
Any customization macros that you use must be defined before you include
any corral headers.

All the functionality described here is provided by:
```cpp
#include <corral/corral.h>
```
If you are using the Boost.Asio integration, you should additionally:
```cpp
#include <corral/asio.h>
```

The names, semantics, and availability of anything in
`corral::detail::` are subject to change without notice.  We will try
to avoid backward-incompatible changes to entities in the top-level
`corral::` namespace, and to flag the changes we can't avoid, but we
are not ready to commit to a deprecation policy as of this initial
release.

## Basic functionality

### Entering corral

* `decltype(auto) run(EventLoop auto& eventLoop, Awaitable auto&& task)`
  : Run a task or other awaitable from non-async context, using the given
  event loop `eventLoop`, which must not already be running. Returns the
  task's result. This is the entry point into corral for your application.
  You must specialize `EventLoopTraits` for the `EventLoop` type you are using,
  in order to teach corral how to interact with it.

### Awaitable\<Ret = _Unspecified_\>

```cpp
template <class T, class Ret = Unspecified>
concept Awaitable = ...;
```
If `Awaitable<T>`, then `co_await std::declval<T>()` is well-formed.
If `Awaitable<T, Ret>`, then it additionally has a result type
covertible to `Ret`.

The header `corral/concepts.h` has extensive documentation about corral's
extensions to the awaitable interface to support cancellation and other
features. There are a number of concepts for detecting whether these
extensions have been implemented, but they are not yet exposed publicly.

### Handle

```cpp
using Handle = std::coroutine_handle<void>;
```
A typedef for `std::coroutine_handle<void>`, which shows up as a parameter
of `await_suspend()` and `await_cancel()`, among other places.

### Task\<T\>

```cpp
template <class T = void>
class [[nodiscard]] Task {
  public:
    using promise_type = ...;

    Task();
    ~Task();
    Task(Task&&) noexcept;
    Task& operator=(Task&&) noexcept;

    explicit operator bool() const;
    Awaitable<T> auto operator co_await();
};
```

A "task handle" that manages an invocation of an async function. This type
shows up primarily as the type returned by an async function whose
result is `T`. `Task<>` is an alias for `Task<void>`.

* `Task<T>::operator bool() const`
  : Test whether this task handle is associated with any task.
  A default-constructed or moved-from `Task` is not associated with
  any task.

* `Awaitable<T> auto Task<T>::operator co_await()`
  : Awaiting on the `Task` starts executing the async function that it's
  wrapping, and suspends the caller until the function completes.

### Nursery

```cpp
class Nursery {
  private:
    Nursery();
    Nursery(Nursery&&);
  public:
    template<class Callable, class... Args>
      requires(Awaitable<std::invoke_result_t<Callable, Args...>>)
    void start(Callable, Args...); /*1*/

    template<clas Ret = Unspecified, class Callable, class... Args>
      requires(Awaitable<std::invoke_result_t<Callable, Args..., TaskStarted<Ret>>)
    Awaitable<Ret> auto start(Callable, Args...); /*2*/

    void cancel();

    size_t taskCount() const noexcept;
    Executor* executor() const noexcept;
};

Task<void> openNursery(Nursery*&);
```

A scope in which a dynamic number of child tasks can run.

* `CORRAL_WITH_NURSERY(nursery) { ... };`
  : Create a nursery that is bound to the current scope. This must be
  written within an async function. The contents of the braces become
  the body of an async lambda that runs as the first task in the nursery.
  It can start other tasks using its `nursery` parameter, which has type
  `Nursery&`. You are free to choose a different name than `nursery` if
  you like. The block must finish with one of `co_return corral::join;` or
  `co_return corral::cancel;`. If you return `join`, the nursery will wait
  patiently for all tasks that were started in it to exit; `cancel` will
  cancel them instead (but still wait for them to exit in response).

* `void Nursery::start(Callable callable, Args... args) /*1*/`
  : Start `std::invoke(callable, args...)` (which must yield an awaitable)
  in the nursery. It will not actually start executing until the next
  `co_await` point is reached. The callable and its arguments will be moved
  into storage that remains valid until the task completes. `std::ref()` and
  `std::cref()` may be used to pass arguments by reference, if the caller
  takes responsibility for the referents' lifetimes; generally only objects
  defined outside the nursery block can safely be passed by reference.

* `Awaitable auto Nursery::start(Callable callable, Args... args) /*2*/`
  : Start `std::invoke(callable, args..., TaskStarted{...})` in the nursery,
  and suspend the caller until the task invokes its `TaskStarted` argument.
  Until its invocation the task is considered to be a child task
  of the spawner: any exceptions raised by the child get reraised as
  the result of `start()`, and cancellation of the spawner is proxied
  to the child task. Upon the invocation of the `TaskStarted` object
  the task is reparented to the nursery, unless the task is being cancelled.
  If the task accepts a `TaskStarted<T>` for a non-void type T, its
  invocation requires a value of that type, which will become the result
  of the `co_await start(...)` expression in the spawner. If this
  result type can't be deduced automatically, which might happen when
  calling a generic lambda for example, you can specify it as a template
  argument to `start()`.

* `void Nursery::cancel()`
  : Request cancellation of all tasks in the nursery, including those that
  have not started yet. (A task started after `cancel()` will still start,
  but is likely to terminate by cancellation the first time it tries to
  await something.)

* `size_t Nursery::taskCount() const noexcept`
  : Returns the number of tasks currently running in the nursery.

* `Executor* Nursery::executor() const noexcept`
  : Returns a pointer to the executor this nursery is using. This is typically
  inherited from the nursery's parent task. After the nursery is closed (all
  tasks have exited and no more will be accepted), the executor will be null.

* `Task<void> openNursery(Nursery*& ptr)`
  : Opens a new nursery, and sets `ptr` to point to it.
  The nursery remains open until the task is cancelled, and `ptr` will be
  reset to null after all tasks in the nursery complete or cancel and
  the nursery closes.

### UnsafeNursery

```cpp
class UnsafeNursery: public Nursery {
public:
    explicit UnsafeNursery(EventLoop auto& eventLoop);
    ~UnsafeNursery();

    void close();
    void asyncClose(std::invocable<> auto continuation);
};
```

A variant of a `Nursery` for certain use cases (typically
[bridging](04_callback_bridging.md) to callback-based code) when one
cannot arrange to have all work be performed in a tree of async tasks
rooted at `corral::run()`.

* `UnsafeNursery(EventLoop auto& eventLoop)`
  : Unlike a regular nursery, an `UnsafeNursery` can be defined as
  a member variable of a class. Like `corral::run()`, it needs a reference
  to the event loop that's being used to drive the tasks in the nursery.

* `void close()`
  : Cancels any tasks still running in the nursery. If any task cannot
  be synchronously cancelled, triggers an assert.
  After `close()` returns, submitting any further tasks to the nursery
  is disallowed (doing so would trigger an assert).

* `~UnsafeNursery()`
  : Calls `close()` before destroying the nursery.

* `void asyncClose(std::invocable<> auto continuation)`
  : Cancels all tasks in the nursery. Once there are no tasks remaining,
  closes the nursery (so no new tasks can be started) and invokes the given
  continuation callback. The continuation callback may safely destroy the
  nursery.


### CBPortal<Ts...>

```cpp
template <class... Ts> class CBPortal {
  public:
    CBPortal();
    CBPortal(CBPortal&&) = delete;
    CBPortal& operator=(CBPortal&&) = delete;

    class Callback {
        Callback();
      public:
        void operator()(Ts... values) const;
    };
};
// The result of awaiting a CBPortal, or an untilCBCalled() that uses the
// portal, depends on the callback signature:
//     co_await CBPortal<>() -> void
//     co_await CBPortal<T>() -> T
//     co_await CBPortal<T1, Ts...>() -> std::tuple<T1, Ts...>
```

A callback portal. See [Bridging to callback code](04_callback_bridging.md)
for more details, including important notes about cancellation handling.

* `Awaitable</* see above */> auto untilCBCalled(auto&& initiator, CBPortal<Ts...>& portal)`
  : Create a `CBPortal<Ts...>::Callback` and pass a reference to it to the
  `initiator` function. Suspend the caller until the callback is invoked,
  and return the argument or arguments that were passed to the callback.

* `Awaitable auto untilCBCalled(auto&& initiator, CBPortal<Ts...>& portal1, CBPortal<Us...>& portal2, ...)`
  : Variant that creates multiple callbacks (one for each portal) and passes
  all of them to the `initiator` function as separate arguments. The caller
  will be suspended until any of the callbacks is invoked. The result type
  matches that of `corral::anyOf(portal1, portal2, ...)`.

* `Awaitable</* see above */> auto CBPortal<Ts...>::operator co_await()`
  : Suspends the caller, waiting for subsequent invocations.

* `Awaitable auto untilCBCalled(auto&& initiator)`
* `Awaitable auto untilCBCalled(auto&& initiator, std::invocable<> auto&& canceller)`
  : Simpler variant for cases where at most one callback invocation is
  expected. The callback signature, and thus the result type of the awaitable,
  is inferred from the types of arguments accepted by `initiator`. It is
  possible to pass an initiator that accepts multiple callbacks in order
  to get an effect analogous to the overload of `untilCBCalled()` that accepts
  multiple portals. If a cancellation is received, calls the `canceller` to
  prevent the callback from occurring; if no `canceller` is provided then
  cancellation will be blocked after the `initiator` has started executing.

### EventLoopTraits

```cpp
class EventLoopID {
  public:
    explicit constexpr EventLoopID(const void*);
};

template <class T, class SFINAE = void>
struct EventLoopTraits {
    static void run(T&);
    static void stop(T&);
    static bool isRunning(T&) noexcept;
    static EventLoopID eventLoopID(T&);
};
```

A traits type that you can specialize for your event loop `T`
in order to teach corral how to interact with it.

* `static void EventLoopTraits<T>::run(T&)`
  : Runs the event loop. Should not return until `stop()` is called.

* `static void EventLoopTraits<T>::stop(T&)`
  : Requests the event loop to stop; `run()` should return soon.

* `static bool EventLoopTraits<T>::isRunning(T&) const noexcept`
  : Returns `true` if the event loop is currently running.
  Only used to guard against re-entering the same event loop;
  if infeasible to implement, it's OK to always return `false`.

* `static EventLoopID EventLoopTraits<T>::eventLoopID(T& eventLoop)`
  : Returns a unique identifier for the event loop that `eventLoop` wraps.
  For the vast majority of use cases, `return EventLoopID(&eventLoop)` will suffice.
  However, if one event loop is running on top of another (through any
  sort of "guest mode"), or if the same event loop might be accessed
  through multiple objects, this should be implemented to look inside
  that abstraction and return the ID of the topmost/"host" event loop.

## Awaitable combiners

* `Awaitable<std::tuple<std::optional<R>...>> auto anyOf(Awaitable<R> auto&&...)`
  : Accepts a series of awaitables which will be run concurrently as
  child tasks of `anyOf()`. When the first one completes, the
  remainder will be cancelled.  The result tuple contains one
  `std::optional<R>` for each `Awaitable<R>` argument (`R` is the
  awaitable's result type), and the optional is engaged if the
  corresponding task produced a result rather than terminating by
  cancellation. While typically there won't be more than one non-empty
  `optional`, it is possible for multiple awaitables to complete
  before the cancellation request takes effect.

* `Awatiable<std::tuple<R>...> auto allOf(Awaitable<R> auto&&...)`
  : Accepts a series of awaitables which will be run concurrently as
  child tasks of `allOf()`. The caller will be suspended until all of
  them complete. The result tuple contains all the task results in the
  order in which they were passed. If any task throws an exception,
  the exception will be reraised as the result of the `allOf()`;
  any value results or additional exceptions are lost. Otherwise, if
  any task terminates by cancellation, the entire `allOf()` will propagate the
  cancellation, discarding any results of tasks that completed normally.

* `Awaitable<std::tuple<std::optional<R>...>> auto mostOf(Awaitable auto&&...)`
  : Like `allOf()`, but only propagates cancellation if all of its
  children terminate by cancellation. Any task that completes normally
  will have its result preserved in the `mostOf()` result tuple; the
  slots corresponding to tasks that were cancelled will be disengaged.

* `Awaitable<std::vector<std::optional<R>>> auto anyOf(AwaitableRange<R> auto&& range)`
* `Awaitable<std::vector<R>> auto allOf(AwaitableRange<R> auto&& range)`
* `Awaitable<std::vector<std::optional<R>>> auto mostOf(AwaitableRange<R> auto&& range)`
  : Variants of the above combiners that accept a `std::range` of awaitables
  instead of a fixed number of them. All of the awaitables must have the same
  result type, denoted `R` in these signatures. The results are returned in a
  `std::vector` rather than a `std::tuple`; semantics are otherwise identical.

## Awaitable wrappers and utilities

* `Awaitable<void> auto suspendForever`
  : An awaitable that never completes, but can be cancelled. Note this is
  a global constant, not a function; write `co_await corral::suspendForever;`.

* `CORRAL_SUSPEND_FOREVER()`
  : A macro equivalent to `co_await suspendForever; std::unreachable()`.
  Using this makes sure the compiler knows that execution can't proceed past
  `suspendForever`, so it won't hassle you about missing return statements etc.

* `Awaitable<void> auto yield`
  : An awaitable that completes immediately, but introduces
  a suspension and cancellation point.
  May also be used to force execution of other tasks that are waiting to run.
  Note that this does *not* check for I/O, only for immediately runnable tasks.

* `Awaitable<void> auto untilCancelledAnd(Awaitable<void> auto&&)`
  : Suspends forever. Upon cancellation, runs the argument awaitable
  as a child task and waits for it to complete. This is useful for
  implementing asynchronous cleanup handlers, via `co_await anyOf(someTask(),
  untilCancelledAnd(cleanupLogic())`.

* ```
  co_await try_([&]() -> Task<> { /*...*/ })
          .catch_([&](const ExceptionType1& e) -> Task<> { /*...*/ }
          .catch_([&](const ExceptionType2& e) -> Task<> { /*...*/ }
          // more catch_-blocks if needed
          .finally([&]() -> Task<> { /*...*/ });
  ```
  An asynchronous equivalent of a classic try-catch-finally block, which allows
  any clause to be asynchronous.

  Finally block is guaranteed to execute regardless of whether the try-block
  exits normally, via an exception, or is cancelled from the outside.
  Any local variables in the try-block will be destroyed before the finally-block
  begins execution.

* ```
  CORRAL_TRY { /*...*/ }
  CORRAL_CATCH(const ExceptionType1& e) { /*...*/ }
  CORRAL_CATCH(const ExceptionType2& e) { /*...*/ }
  CORRAL_FINALLY { /*...*/ };
  ```
  Same as above, but with another, more laconic, syntax.
  Note the trailing semicolon, which is required in this case.

* `Awaitable<T> auto noncancellable(Awaitable<T> auto&&)`
  : Wraps an awaitable, shielding it from cancellation (including early
  cancellation).

* `Awaitable<T> auto disposable(Awaitable<T> auto&&)`
  : Wraps an awaitable and marks its value as safe to discard
  if the awaitable is cancelled. This allows a non-cancellable operation,
  such as one that is implemented by a library that isn't aware of corral's
  cancellation extensions, to nevertheless serve as a cancellation point.
  It is only likely to be useful if the wrapped operation completes in
  a relatively short time without any side effects.

* `Awaitable auto yieldToRun(std::invocable<> auto fn)`
  : Runs `fn()` with the current task suspended, returning its result.

* `auto then(auto callable)`
  : Allows chaining multiple awaitables into one in a monadic fashion,
  without having to allocate a frame on the heap for a glue coroutine.
  The resulting value of `then()` can be attached to an awaitable through `opreator|()`
  (like `sem.lock() | then([](auto& lk) { return sleepFor(io, 3s); })`)
  to form an awaitable which would first run the first child awaitable,
  and upon its completion call `callable` (passing it the result of the awaitable)
  to obtain the second awaitable, then start it and run it to completion.
  If the callable accepts the result by reference, its lifetime will be extended
  until the second awaitable completes.

## Synchronization primitives

### Event

```cpp
class Event {
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;
  public:
    void trigger();

    Awaitable<void> auto operator co_await();
    Awaitable<void> auto get();
    bool get() const noexcept;
    bool triggered() const noexcept;
```

A level-triggered event, capable of a one-time, one-way transition
from the "not triggered" to the "triggered" state. Tasks can wait for
the event to be in the triggered state.

* `void Event::trigger()`
  : Triggers the event, waking up all suspended tasks.

* `bool Event::triggered() const noexcept`
  : Returns whether the event has been triggered.

* `Awaitable<void> auto Event::operator co_await()`
  : Awaiting on the event suspends the caller until it is triggered.

* `bool Event::get() const noexcept`
* `Awaitable<void> auto Event::get()`
  : The Event awaitable type is contextually convertible to bool, returning
  the same value as `triggered()`. A method `get()` is provided for use in
  generic contexts; it returns `triggered()` when run on a const Event,
  or `operator co_await()` when run on a non-const Event.

### ParkingLot

```cpp
class ParkingLot {
    ParkingLot(ParkingLot&&) = delete;
    ParkingLot& operator=(ParkingLot&&) = delete;
  public:
    Awaitable<void> auto park();
    void unparkOne();
    void unparkAll();
    bool empty() const noexcept;
};
```

A wait queue for async tasks, which can also be used to implement an
edge-triggered event (i.e., waking up waiters when some condition *just
now occurred*, as opposed to `Event` which allows waiting for the condition to
*have occurred*). Some concurrency frameworks call this a condition variable,
but ours doesn't have an associated lock (with cooperative multitasking
it's generally not necessary).

* `Awaitable<void> auto ParkingLot::park()`
  : Suspends the caller until the next `unparkOne()` or `unparkAll()`.

* `void ParkingLot::unparkOne()`
* `void ParkingLot::unparkAll()`
  : Resumes one or all currently parked tasks; no-op if there aren't any.
  Has no effect on any tasks `park()`ed afterwards.

* `bool ParkingLot::empty() const noexcept`
  : Returns whether there are no currently parked tasks.

### Semaphore

```cpp
class Semaphore {
    Semaphore(Semaphore&&) = delete;
    Semaphore& operator=(Semaphore&&) = delete;
  public:
    class Lock; // movable RAII guard

    explicit Semaphore(size_t initial = 1);
    size_t value() const noexcept;

    Awaitable<void> auto acquire();
    void release();
    Awaitable<Lock> auto lock();
};
```

A semaphore, which can also be used to implement a lock. It maintains
an internal counter, which is reduced by `acquire()` and increased
by `release()`, and is not allowed to go below zero. `acquire()` when
the counter is zero will block until it isn't, then decrement it.

* `Semaphore::Semaphore(size_t initial = 1)`
  : Constructs a semaphore with the given initial value.

* `size_t Semaphore::value() const noexcept`
  : Returns the current value.

* `Awaitable<void> auto Semaphore::acquire()`
  : Decrements the stored value, suspending the caller if it's zero.

* `void Semaphore::release()`
  : Increments the stored value, waking up one waiter if applicable.

* `Awaitable<Semaphore::Lock> auto Semaphore::lock()`
  : Same as `acquire()`, but returns a RAII lock object which `release()`es
  the semaphore when the lock is destroyed.

### Shared

```cpp
template <class T>
class Shared {
  public:
    Shared();
    Shared(T&&);
    Shared(std::in_place_t, auto&&... args);

    Object* get() const;
    explicit operator bool() const;
    Object& operator*() const;
    Object* operator->() const;

    bool closed() const noexcept;

    Awaitable auto operator co_await();
    Awaitable auto asOptional();
};
```

A "shared task" which wraps a task so it can be awaited by multiple
coroutines concurrently. `T` is the type of the awaitable for the
task, which would typically be some `corral::Task<R>`, but other awaitable
types are also supported.

* `Shared<T>::Shared(T&&)`
  : Constructor.

* `Shared<T>::Shared(std::in_place_t, auto&&...)`
  : In-place constructor, suitable for non-movable awaitable types.

* `T* Shared<T>::get() const`
* `explicit Shared<T>::operator bool()`
* `T& Shared<T>::operator*() const`
* `T* Shared<T>::operator->() const`
  : Pointer-like accessors to the shared awaitable object.

* `Awaitable auto Shared<T>::operator co_await()`
  : Suspends the caller until the shared task completes.
  The first awaiting task will start it running.
  Cancellation of this awaitable succeeds immediately and has no
  effect on the shared task if there are other awaiting tasks still active.
  Cancellation of the last awaiting task will cancel the shared task
  and wait for it to complete. If more awaiters join the shared task
  after the decision to cancel it has already been made, they will
  cause an assertion failure since there is no way to construct a
  return value for them to use. Use `asOptional()` to guard against
  this possiblity.

* `Awaitable auto Shared<T>::asOptional()`
  : Same as above, except that if the decision to cancel the shared
  task was made before this awaiter started awaiting, it will receive
  a result of `std::nullopt` instead of an assertion failure.

### Value<T>

```cpp
template <class T>
class Value {
    Value(Value&&) = delete;
    Value& operator=(Value&&) = delete;

    class Comparison;

  public:
    Value();
    explicit Value(T value);

    const T& get() const noexcept;
    operator const T&() const noexcept;

    void set(T value);
    Value& operator=(T value);
    T modify(std::invocable<T&> auto&& fn);

    Awaitable<T> auto untilMatches(std::invocable<const T&> auto&& pred);
    Awaitable<T> auto untilEquals(T expected);

    Awaitable<std::pair<T, T>> auto untilChanged(std::invocable<const T&, const T&> auto&& pred);
    Awaitable<std::pair<T, T>> auto untilChanged(T from, T to);
    Awaitable<std::pair<T, T>> auto untilChanged();

    T operator++();
    T operator++(int);
    T operator+=(auto&&);
    // ...etc...

    Comparison operator==(auto&&);
    bool operator==(auto&&) const;
    // ...ditto for other comparisions...

    auto operator<=>(auto&&) const;
};

template<class T>
class Value<T>::Comparison {
  public:
    operator bool() const;
    friend Awaitable auto until(Comparision&&);
};
```

A variable that allows tasks to block until its value, or a transition
thereof, satisfies a given condition.

* `const T& Value<T>::get() const noexcept`
* `Value<T>::operator const T&() const noexcept`
* `void Value<T>::set(T)`
* `Value<T>& Value<T>::operator=(T)`
  : Get or set the value.

* `T Value<T>::modify(std::invocable<T&> auto&& fn)`
  : Modifies the value in-place.

* `Awaitable<T> auto Value<T>::untilMatches(std::invocable<const T&> auto&& pred)`
  : Suspends the caller until the stored value matches the predicate
  (or resumes immediately if it already does).
  The awaitable's result is the value that matched the predicate.
  Note that the value stored in the `Value` object might have been further
  changed (and might not match the predicate anymore) before the resumed task
  had a chance to observe it.

* `Awaitable<T> auto Value<T>::untilEquals(T value)`
  : Suspends the caller until the stored value equals `value`,
  or resumes immediately if it already does. Caveats under `untilMatches()`
  apply here too.

* `Awaitable<std::pair<T, T>> auto Value<T>::untilChanged(std::invocable<const T&, const T&> auto&& pred)`
  : Suspends the caller until a transition of the stored value
  satisfies `pred(old, new)`. The awaitable's result is a pair of the
  old and new values that satisfied the predicate. Note that the value
  may have changed further before the task could be resumed.

* `Awaitable<std::pair<T, T>> auto Value<T>::untilChanged(T from, T to)`
  : Same as above, but waits for a transition from `from` to `to`.

* `Awaitable<std::pair<T, T>> auto Value<T>::untilChanged()`
  : Same as above, but waits for any nontrivial transition (change from
  `x` to `y` where `x != y`).

* `T::operator++()` _etc_
  : Proxies the operator to the underlying value, triggering any awaiters as appropriate.

* `Comparision operator==(auto&& rhs)` _etc_
  : Returns a proxy object which is convertible to bool (`bool b = (v == 42)`)
  or can be turned into an awaitable through `until()` friend function (`co_await until(v == 42)`).

* `bool operator==(auto&& rhs) const` _etc_
  : Proxies the comparison to the underlying value.

### Channel<T>

```cpp
template <class T>
class Channel {
  public:
    Channel();
    explicit Channel(size_t maxSize);

    bool closed() const noexcept;

    // Reading

    Awaitable<std::optional<T>> auto receive();
    std::optional<T> tryReceive();

    size_t size() const noexcept;
    bool empty() const noexcept;

    class ReadHalf;
    ReadHalf& readHalf();

    // Writing

    template <class U> Awaitable<bool> auto send(U&&);
    template <class U> bool trySend(U&&);
    void close();

    size_t space() const noexcept;
    bool full() const noexcept;

    class WriteHalf;
    WriteHalf& writeHalf();
};
```

An ordered communication channel for sending objects of type T between tasks.

* `Channel<T>::Channel(size_t maxSize)`
  : Constructs a bounded channel, capable of holding up to `maxSize` objects
  not yet receieved. An attempt to send further objects will block the calling
  task until some objects are retrieved from the channel.
  Zero-sized channels are disallowed; even if a reader is always available,
  every object must still go through the buffer, so a channel with `maxSize == 0`
  would be unusable.

* `Channel<T>::Channel()`
  : Constructs an unbounded channel, whose `send()` never blocks; its underlying
    buffer can grow dynamically as needed.

* `Awaitable<bool> auto Channel<T>::send(std::convertible_to<T> auto&&)`
  : Delivers an object to the channel. If the channel is closed (or becomes closed
  while waiting for buffer space), returns false. Otherwise, returns true once
  the object has been accepted, which may not be immediate if a bounded channel's
  buffer is full.

* `bool Channel<T>::trySend(std::convertible_to<T> auto&&)`
  : Non-blocking version of the above. Returns false if the channel
  is closed, or if no space is available in a bounded channel.

* `void Channel<T>::close()`
  : Indicates that no further objects will be sent through the channel. Any queued
  `send()` and `receive()` tasks will complete with a result that indicates failure.
  Objects already accepted by the channel remain available for future calls to
  `receive()` or `tryRecieve()`.

* `Awaitable<std::optional<T>> auto Channel<T>::receive()`
  : Retrieves and returns an object from the channel, suspending the caller
  if no objects are immediately available. Returns `std::nullopt` if the channel
  is closed and all of its objects have been consumed.

* `std::optional<T> Channel<T>::tryReceive()`
  : Non-blocking version of the above. May return `std::nullopt` if no objects
  are currently available.

* `size_t Channel<T>::size() const noexcept`
  : Returns the number of objects immediately available to read from this channel,
  i.e., the number of times in a row you can call `tryReceive()` successfully.

* `bool Channel<T>::empty() const noexcept`
  : Returns true if there are no objects immediately available, i.e., whether
  `tryReceive()` would return `std::nullopt`.

* `size_t Channel<T>::space() const noexcept`
  : Returns the amount of free space in the channel buffer, i.e., the number of times
  in a row you can call `trySend()` successfully. Returns 0 for closed channels,
  and `std::numeric_limits<size_t>::max()` for unbounded channels.

* `bool Channel<T>::full() const noexcept`
  : Returns true if there is no free space in the channel buffer (or the channel
  is closed), i.e., whether `trySend()` would fail.

* `Channel<T>::ReadHalf& Channel<T>::readHalf()`
* `Channel<T>::WriteHalf& Channel<T>::writeHalf()`
  : Returns a reference to a subobject that exposes only the read-related methods,
  or only the write-related methods, of the channel. (These are defined as base classes
  of `Channel`, so a `Channel<T>&` is also implicitly convertible to them.) Accepting
  a `ReadHalf&` as a parameter can be used to indicate (and guarantee) that
  the channel is only read from, but not written to; and vice versa for `WriteHalf`.


## Multithreading support

### ThreadPool

```cpp
class ThreadPool {
  public:
    class CancelToken;

    template<class EventLoopT>
    ThreadPool(EventLoopT& eventLoop, unsigned threadCount);

    template<class F, class... Args>
      requires (std::invocable<F, Args...>
             || std::invocable<F, Args..., CancelToken>)
    Awaitable auto run(F&&, Args&&...);
};

class ThreadPool::CancelToken {
  public:
    explicit operator bool() noexcept;
};
```

A tool for offloading CPU-bound work, allowing it to run concurrently with the main
(event-dispatching) thread.

* `template<class EventLoopT> ThreadPool(EventLoopT& eventLoop, unsigned threadCount)`
  : Creates a thread pool featuring a fixed amount of threads and delivering results
    to thread running `eventLoop`.
    A specialization of `ThreadNotification` for the event loop type must be defined
    (see below).

* `template<class F, class... Args> Awaitable auto run(F&&, Args&&...)`
  : Submits a regular (synchronous) function to the thread pool, to be executed
    on one of the worker threads. Suspends the caller until the function returns,
    and returns the result of its execution (reraising any exceptions).
    Can only be called from the main thread.
    The function may take an optional `ThreadPool::CancelToken` argument, which
    can be queried periodically for the cancellation status of the awaitable
    (see below).

* `ThreadPool::CancelToken::operator bool()`
  : Returns true if cancellation of the coroutine suspended on `ThreadPool::run()`
    has been requested. Also marks the cancellation request confirmed,
    so if returned true, any return value or generated exception will be discarded.
    The coroutine will remain suspended until the function returns.


### ThreadNotification

```cpp
template<class EventLoopT>
class ThreadNotification {
  public:
    ThreadNotification(EventLoopT&, void (*fn)(void*), void* arg);
    void post(EventLoopT&, void (*fn)(void*), void* arg);
};
```

A traits-like class for delivering completion events from worker threads
to the main thread.

* `ThreadNotification(EventLoopT& eventLoop, void (*fn)(void*), void* arg)`
  : Constructor.
    `eventLoop`, `fn`, and `arg` will match those passed to all `post()` calls
    (see below).

* `void post(EventLoopT& eventLoop, void (*fn)(void*), void* arg)`
  : Called from worker threads of a thread pool, and should arrange `fn(arg)`
    to be run soon on the main thread.
    Multiple calls may or may not be coalesced into a single `fn(arg)` invocation.


## Low-level interfaces

### Introspection tools

```cpp
template <std::output_iterator<uintptr_t> OutIt>
OutIt collectAsyncStackTrace(OutIt out);

struct TreeDumpElement {
    std::variant<uintptr_t /*pc*/,
                 const char* /*name*/,
                 const std::type_info* /*type*/> value;
    const void* ptr;
    int depth;
};

template <std::output_iterator<TreeDumpElement> OutIt>
Awaitable<OutIt> auto dumpTaskTree(OutIt out);

template<Awaitable T>
Awaitable auto annotate(std::string annotation, T&&);
```

* `OutIt collectAsyncStackTrace(OutIt out)`
  : Produce an async stack trace, reflecting the series of program
  addresses at which `co_await` expressions led to the currently running
  task. Successive addresses are written as `uintptr_t` to `*out++`, with the
  innermost one (most recent `co_await`) first.
  They can be converted to human-readable function names or source locations
  using a tool such as `addr2line` or `backtrace_symbols()`.

* `Awaitable<OutIt> auto dumpTaskTree(OutIt out)`
  : Produce the entire task tree for the current async "universe"
  (corresponding to a single `Executor`). This will show every async
  task in your program if you run it underneath `corral::run()`, but
  if you run it underneath an `UnsafeNursery` then it will only show
  tasks underneath that same nursery. Successive tree nodes are
  written in preorder as `TreeDumpElement`s to `*out++`; use
  `TreeDumpElement::depth` to tell whether a node's successor is its
  child, sibling, or parent. Each `TreeDumpElement` contains one of:
  the program address of a `co_await` statement in an async function;
  a textual name describing an awaitable that implements
  `await_introspect()`; or a `std::type_info*` indicating the type
  of an awaitable that does not implement `await_introspect()`.
  `ptr` holds the address of the coroutine frame or the awaitable.

* `Awaitable<T> auto annotate(std::string annotation, Awaitable<T> auto&&)`
  : Allows annotating the awaitable with a custom string to appear
  in the async task tree (see above). The tree will have an artificial
  node with a matching name, with the only child being the wrapped awaitable.

### Executor

```cpp
class Executor {
  public:
    template <class T>
    void runSoon(void (*fn)(T*), T* arg);
    void drain();
    void capture(std::invocable<> auto&& fn);
};
Awaitable<Executor*> auto getExecutor();
```

A runner for task steps and other operations that shouldn't nest.
This is a relatively low-level interface which is widely used internally
but is rarely needed by end users; see `corral/Executor.h` for more on
its model and rationale.

There are two ways to get ahold of the executor:

* Within a task: `Executor* ex = co_await corral::getExecutor();`

* For an awaitable: implement the optional method
  `void await_set_executor(Executor*) noexcept`, which corral will call before
  `await_suspend()` if it's defined.

Once you have the executor, you can use it as follows:

* `template <class T> void Executor::runSoon(void (*fn)(T*), T* arg)`
  : Add `fn(arg)` to the list of operations to run. It will run immediately
  unless an executor is already running, in which case it will be executed
  on the next executor loop.

* `void Executor::drain()`
  : Immediately execute everything that has been scheduled so far. Unlike
  `runSoon()`, this method won't worry about nesting of operations, so you
  can use it to violate the rule that tasks only switch at a `co_await`.

* `void Executor::capture(std::invocable<> auto&& fn)`
  : Synchronously run `fn()`, then immediately run everything that it scheduled
  on the executor before returning. (Second-order effects, i.e., operations
  that were scheduled due to running the operations that were scheduled by
  `fn()`, are added to the executor like normal and will not necessarily run
  before `capture()` returns.) This method can be used to
  violate the rule that tasks only switch at a `co_await`; see the
  implementation of CBPortal for a rare example of circumstances that might
  necessitate it.
