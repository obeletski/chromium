# Threading in the browser and renderer processes

Is threading allowed? Yes. Are mutexes used? Yes, but far less than you would
expect. Are condition variables used? **Almost never** — 18 references across
the whole tree, and nearly all of them are inside `base/` itself.

This note explains why the numbers come out that way, what you are supposed to
write instead, and what the compiler and runtime will stop you from doing.

Counts are from `*.cc`/`*.h` under `base/ chrome/ content/ components/ ui/
third_party/blink/renderer/`, on `chrome/VERSION` 153.0.8005.0.

**Companion notes in this directory:**

* [`chromium-cpp-idioms.md`](chromium-cpp-idioms.md) — the C++ dialect. Its §2
  covers why `<thread>`, `<mutex>` and `<condition_variable>` are banned outright.
* [`chromium-design-patterns.md`](chromium-design-patterns.md) — object-graph
  patterns; this note is the one its "does not cover" list points at for
  sequences.
* [`chromium-linux-processes-and-ipc.md`](chromium-linux-processes-and-ipc.md) —
  where the IO thread's `epoll` loop and the Mojo channel come from.
* [`chromium-android-architecture.md`](chromium-android-architecture.md) — the
  process model these threads live inside.

---

## 1. The short answers

| Question | Answer |
|---|---|
| Is threading allowed? | Yes, but you rarely create a thread. `base::Thread` appears 159 times; `base::ThreadPool::` 1,952. |
| Are mutexes used? | Yes — `base::AutoLock` 2,275 — but task posting outnumbers them ~7:1 (`SequencedTaskRunner` + `SingleThreadTaskRunner` = 15,385). |
| Condition variables? | **18 references, tree-wide.** Effectively a `base/`-internal primitive. |
| Reader-writer locks? | None. "Chrome doesn't expose reader-writer locks." |
| `std::mutex`, `std::thread`, `std::condition_variable`? | **Banned.** So is `<future>`, `<latch>`, `<semaphore>`, `<barrier>`, `<stop_token>`. |
| So what do I write? | A **sequence**: post tasks to a `base::SequencedTaskRunner` and assert with `SEQUENCE_CHECKER` (1,342 uses). |

The governing sentence is in
[`docs/threading_and_tasks.md`](https://github.com/obeletski/chromium/blob/floating-window/docs/threading_and_tasks.md):

> "Usage of locks is discouraged in Chrome. Sequences inherently provide
> thread-safety. Prefer classes that are always accessed from the same sequence
> to managing your own thread-safety with locks."

---

## 2. The vocabulary

Named before the diagrams use them, because "sequence" is the term that does the
work and it is not a standard one:

* **Thread** — an OS thread. Chromium has few and names most of them.
* **Sequence** — a *virtual* thread: a guarantee that a set of tasks runs one at
  a time, in posting order, with each task seeing the previous one's writes.
  Successive tasks may run on **different physical threads**.
* **`base::SequencedTaskRunner`** — the handle you post to for that guarantee.
* **`base::SingleThreadTaskRunner`** — stronger: same physical thread every
  time. It *is-a* `SequencedTaskRunner`. Needed only for thread-affine
  dependencies.
* **Task** — a `base::OnceClosure` on a queue. The unit of everything.
* **`base::ThreadPool`** — the shared pool of general-purpose worker threads that
  backs most sequences.
* **`SEQUENCE_CHECKER` / `THREAD_CHECKER`** — debug-build members asserting that
  every access came from the right sequence or thread.
* **Blocking call** — anything that parks the thread off-CPU: file I/O, sockets,
  waiting on an event. **Banned by default**, per thread (§6).

The lexicon in `threading_and_tasks.md` adds the adjective that matters most:

> "**Thread-unsafe**: The vast majority of types in Chrome are thread-unsafe
> (by design)."

That is the default you should assume for any class you meet.

---

## 3. The model: sequences instead of locks

The idea is that mutual exclusion is a *scheduling* problem, not a *locking*
problem. If two pieces of code never run at once, they need no lock.

```cpp
class A {
 public:
  A() {
    // Do not require accesses to be on the creation sequence.
    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  void AddValue(int v) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    values_.push_back(v);
  }

 private:
  SEQUENCE_CHECKER(sequence_checker_);
  // No lock required, because all accesses are on the same sequence.
  std::vector<int> values_;
};
```

The non-obvious guarantee, and the reason this is not just single-threading with
extra words:

> "Tasks posted to the same sequence will run in sequential order. After a
> sequenced task completes, the next task may be picked up by a **different
> worker thread**, but that task is guaranteed to see any side-effects caused by
> the previous one(s) on its sequence."

So a sequence is thread-safe without being thread-affine. You get the reasoning
simplicity of one thread and the scheduling freedom of a pool. The tree strongly
prefers this: `SEQUENCE_CHECKER` 1,342 against `THREAD_CHECKER` 247.

```mermaid
flowchart TD
  Q0{"Two pieces of code<br/>touch the same state"}
  Q0 -->|"they can run<br/>one after another"| SEQ["<b>Use a sequence</b><br/><small>post to a SequencedTaskRunner<br/>assert with SEQUENCE_CHECKER</small>"]
  Q0 -->|"they must run<br/>genuinely in parallel"| Q1{"How big is<br/>the shared state?"}
  Q1 -->|"one word"| AT["std::atomic<br/><small>815 uses, allowed</small>"]
  Q1 -->|"a data structure"| LK["<b>base::Lock</b><br/><small>hold it briefly, annotate GUARDED_BY</small>"]
  Q0 -->|"a whole object<br/>owned by another sequence"| SB["base::SequenceBound&lt;T&gt;<br/><small>247 uses</small>"]

  NEVER["std::mutex, std::thread,<br/>std::condition_variable, std::future"]
  NEVER --> BAN["<b>banned outright</b><br/><small>styleguide c++-features.md</small>"]

  classDef good fill:#bfe3d0,stroke:#1e6b45,stroke-width:1.5px,color:#0d3b26
  classDef ok fill:#d9e4f5,stroke:#2b4c7e,color:#12274a
  classDef bad fill:#f3c2c7,stroke:#96222e,color:#4d1219
  class SEQ,SB good
  class AT,LK ok
  class NEVER,BAN bad
```

`base::SequenceBound<T>` deserves a mention because it makes the pattern
type-safe: it owns a `T` on a given sequence and only lets you reach it by
posting, so a cross-sequence access is a compile error rather than a DCHECK.

---

## 4. The threads that actually exist

Every process has a main thread, an IO thread, a few special-purpose threads,
and a pool. What the first two mean differs by process.

```mermaid
flowchart TB
  subgraph BP["Browser process"]
    BUI["BrowserThread::UI<br/><small>the main thread. Runs the UI.<br/>Never joined; stops taking tasks at shutdown</small>"]
    BIO["BrowserThread::IO<br/><small>non-blocking I/O: all IPC arrives here</small>"]
    BPOOL["base::ThreadPool<br/><small>everything else</small>"]
  end

  subgraph RP["Renderer process"]
    RMAIN["Blink main thread<br/><small>runs most of Blink, JS, DOM, layout</small>"]
    RIO["IO thread<br/><small>Mojo messages arrive here</small>"]
    ROTHER["compositor and worker threads<br/><small>plus the pool</small>"]
  end

  BIO -.->|"Mojo over a socket"| RIO
  BIO -->|"routes to the bound sequence"| BUI
  RIO -->|"routes to the bound sequence"| RMAIN

  classDef main fill:#bfe3d0,stroke:#1e6b45,stroke-width:1.5px,color:#0d3b26
  classDef io fill:#f6e7c1,stroke:#8a6d1f,color:#4a3a08
  class BUI,RMAIN main
  class BIO,RIO io
```

Two things this makes concrete.

**The IO thread is not where your code runs.** All IPC arrives there, but the
handler is bound to some other sequence and the IO thread just routes to it —
which is why a slow message handler does not stall the process's other IPC. The
IO thread's own loop is the `epoll` loop described in §4 of the
[processes-and-IPC note](chromium-linux-processes-and-ipc.md).

**`BrowserThread` is closed for extension.** The header says so at the enum
([`browser_thread.h:96`](https://github.com/obeletski/chromium/blob/floating-window/content/public/browser/browser_thread.h#L96)):
"NOTE: do not add new threads here. Instead you should just use..." — the pool.
That is why `base::Thread` is down to 159 uses: creating a physical thread is
now the rare, justified case rather than the default.

---

## 5. Locks: allowed, common, and narrowly scoped

Locks are not forbidden — `base::AutoLock` at 2,275 uses is not a rounding
error. What is discouraged is using them as the *primary* thread-safety strategy.

```cpp
class C {
 private:
  base::Lock lock_;
  Data data_ GUARDED_BY(lock_);   // 827 uses of GUARDED_BY tree-wide
};

void C::Update() {
  base::AutoLock auto_lock(lock_);   // RAII; there is no manual Acquire/Release idiom
  data_.Mutate();
}
```

Three details worth knowing:

* **`base::Lock` is a `pthread_mutex` on POSIX**, configured in
  [`lock_impl_posix.cc:113`](https://github.com/obeletski/chromium/blob/floating-window/base/synchronization/lock_impl_posix.cc#L113)
  with `PTHREAD_MUTEX_ERRORCHECK` — so relocking one you already hold is
  diagnosed rather than deadlocking — and `PTHREAD_PRIO_INHERIT` where
  available, which limits priority inversion when a background thread holds a
  lock the UI thread wants.
* **`GUARDED_BY` is checked by the compiler**, not by convention. It is a Clang
  thread-safety annotation, so reading `data_` without holding `lock_` is a
  build error in a properly annotated class.
* **There are no reader-writer locks.** The lexicon explains what to do instead:
  a "thread-compatible" global is initialised once — in single-threaded startup
  or through `base::NoDestructor` — and immutable forever after.

The doc's rule for when a lock is right:

> "Locks should only be used to swap in a shared data structure that can be
> accessed on multiple threads. If one thread updates it based on expensive
> computation or through disk access, then that slow work should be done without
> holding the lock."

Compute outside, swap inside. Never hold a lock across I/O — which, as §6 shows,
the runtime will also try to stop you doing.

---

## 6. Why condition variables are almost unused

18 references, and the file list is the answer: `base/` internals
(`waitable_event_posix.cc`, `lock_impl.h`, `thread_restrictions.h`), performance
tests, chromedriver, and a Nearby-Connections platform shim. **Essentially no
product code constructs one.**

That is not an accident of taste. Waiting is *restricted at runtime*.
[`thread_restrictions.h:38`](https://github.com/obeletski/chromium/blob/floating-window/base/threading/thread_restrictions.h#L38)
puts condition variables in the same banned category as disk I/O:

> "**Waiting on a //base sync primitive**: Refers to calling one of these
> methods: `base::WaitableEvent::*Wait*`, `base::ConditionVariable::*Wait*`,
> `base::Process::WaitForExit*`"

So on a normal thread, `ConditionVariable::Wait()` does not merely block — it
trips a check. To be allowed to wait at all you must either be on a pool task
declared with the right traits, or hold an explicit scoped allowance:

| Escape hatch | Meaning |
|---|---|
| `base::MayBlock()` task trait | this pool task may block, so schedule it accordingly |
| `base::WithBaseSyncPrimitives()` trait | this pool task may wait on a `WaitableEvent` or `ConditionVariable` |
| `ScopedAllowBlocking` (1,638 uses) | a narrow, reviewed exception in non-pool code |
| `ScopedAllowBaseSyncPrimitives` | the same, for sync-primitive waits |
| `ScopedBlockingCall` (658) | *annotates* an unavoidable blocking call so the pool can spin up another worker |

Note the asymmetry in the counts: `ScopedAllowBlocking` at 1,638 versus
`ScopedAllowBaseSyncPrimitives` at a handful. Blocking on **I/O** is a common,
grudgingly-permitted thing. Blocking on **another thread** is not, because it is
the shape that produces deadlocks and jank, and because a callback almost always
expresses the same intent without a parked thread.

Where a wait genuinely is needed, `base::WaitableEvent` (1,592) is the primitive
reached for rather than a condition variable — it composes with
`WaitableEventWatcher` so the wait can become a callback instead of a block.

```mermaid
flowchart TD
  W["Code wants to wait<br/>for something"]
  Q1{"Can it be a callback<br/>or a posted reply?"}
  W --> Q1
  Q1 -->|"yes, almost always"| CB["<b>PostTaskAndReply</b><br/>or a OnceCallback<br/><small>nothing blocks</small>"]
  Q1 -->|"no"| Q2{"Running on a<br/>ThreadPool task?"}
  Q2 -->|"yes"| TR["declare MayBlock and<br/>WithBaseSyncPrimitives traits"]
  Q2 -->|"no"| SA["ScopedAllowBlocking or<br/>ScopedAllowBaseSyncPrimitives<br/><small>narrow, reviewed, justified</small>"]
  TR --> EV["base::WaitableEvent<br/><small>1592 uses</small>"]
  SA --> EV
  EV -.->|"rarely"| CV["base::ConditionVariable<br/><small>18 refs, mostly inside base</small>"]

  classDef good fill:#bfe3d0,stroke:#1e6b45,stroke-width:1.5px,color:#0d3b26
  classDef rare fill:#f6e7c1,stroke:#8a6d1f,color:#4a3a08
  class CB good
  class CV,SA rare
```

---

## 7. What the toolchain enforces

Little of this rests on reviewer diligence:

| Rule | Enforced by |
|---|---|
| No `std::thread`, `std::mutex`, `std::condition_variable`, `<future>`, `<latch>`, `<semaphore>` | the style guide's banned list, and there is no `#include` in `base/` to make them convenient |
| Access from the wrong sequence | `SEQUENCE_CHECKER` — `DCHECK` in debug and `dcheck_always_on` builds |
| Reading a field without its lock | `GUARDED_BY` — a Clang thread-safety **compile** error |
| Blocking on a thread that forbids it | `thread_restrictions.h` — a runtime check |
| Binding a raw pointer into a cross-thread callback | `base::Bind*` refuses; you must write `base::Unretained`, a `WeakPtr`, or `RetainedRef` (see §3 of the idioms note) |

The last row is the connection to the callback machinery: because tasks are
`base::OnceClosure`s, every cross-thread hand-off is also a lifetime decision,
and the bind wrappers make each one visible.

---

## 8. The rest of the cabinet

Sections 1–7 cover what you reach for daily. The rest of `base/threading/` and
`base/task/` is worth skimming once, because several of these solve a problem
you would otherwise solve badly by hand.

### Posting and replying

| Class | Uses | What it is for |
|---|---|---|
| `PostTaskAndReplyWithResult` | 1,221 | Run a function on another sequence, get its **return value** delivered back on yours. The workhorse for "do the slow thing elsewhere, then continue here". |
| `base::BindPostTask` | 612 | Wrap a callback so that, wherever it is eventually run, it hops to a chosen task runner first. The standard way to hand a callback to code that does not know your sequence. |
| `base::CancelableTaskTracker` | 800 | Post tasks you can cancel **by id**, from any sequence, with the guarantee that a cancelled task's reply never runs. Where a `WeakPtr` is not enough because cancellation must be explicit rather than lifetime-driven. |
| `DeferredSequencedTaskRunner` | 62 | Queue tasks now, start running them later. Used during startup, before the real target sequence exists. |
| `base::PostJob` / `JobHandle` | 7 / 71 | Parallel work-splitting: a job with N worker slots that the pool grows and shrinks based on remaining work. For data-parallel loops, not for one-off tasks. |

### Threads, when you really do need one

| Class | Uses | What it is for |
|---|---|---|
| `base::PlatformThread` | 971 | The lowest layer — thread creation, `PlatformThread::CurrentId()`, `Sleep()`, thread names and priorities. The high count is mostly identity and naming, not thread creation. |
| `base::Thread` | 159 | A thread that owns a message loop, so you can post to it. What you create when a dependency is genuinely thread-affine. |
| `base::SimpleThread` / `DelegateSimpleThread` | 26 / 33 | A thread with **no** message loop — a plain `Run()` that exits. For a self-contained loop that never needs to receive tasks. |
| `base::Thread::Options` | 75 | Where the message-pump type, stack size and `ThreadType` are chosen at creation. |
| `base::ThreadType` | 230 | Scheduling priority as an enum — `kBackground` through `kDisplayCritical` / `kRealtimeAudio` — rather than raw nice values. |
| `ScopedThreadPriority` | 13 | Temporarily raise priority for a scope, e.g. around a known-contended section. |

### Per-thread and per-sequence storage

| Class | Uses | What it is for |
|---|---|---|
| `SequenceLocalStorageSlot` | 93 | "Values stored and retrieved from a sequence. Values are deleted when the sequence is deleted." The sequence-shaped answer to thread-local storage, and the one to prefer. |
| `base::ThreadLocalStorage` | 12 | Genuine TLS. Rare, because a value tied to a physical thread is usually the wrong scope in a pool-backed world. `thread_local` covers most remaining cases. |

### Diagnostics

| Class | Uses | What it is for |
|---|---|---|
| `base::HangWatcher` | 85 | Instantiate a `WatchHangsInScope` and the watcher reports if that scope takes longer than a timeout. How jank becomes a crash report rather than a mystery. |
| `base::Watchdog` | 3 | An older, coarser alarm: arm it, disarm it, and it fires if you did not. |
| `THREAD_COLLISION_WARNER` | 3 | Detects two threads entering a section that was assumed to be single-threaded — a lighter, non-fatal cousin of `SEQUENCE_CHECKER` for hot paths. |
| `base::CurrentThread` | 77 | Introspection on the current message loop: is one running, add a `TaskObserver`, check whether the current thread runs tasks at all. |

### Small primitives it is easy to miss

| Class | Uses | What it is for |
|---|---|---|
| `base::AtomicFlag` | 44 | A one-way boolean: set once, read from anywhere. Narrower and clearer than a `std::atomic<bool>` because it cannot be unset. |
| `base::AtomicRefCount` | 17 | The counter behind refcounting, occasionally used directly. |
| `base::ScopedClosureRunner` | 830 | Not threading-specific, but the usual way to guarantee a completion callback runs on every exit path — including the early returns that make threaded code go wrong. |
| `base::WaitableEventWatcher` | — | Turns "wait for this event" into "run this callback when the event fires", which is how a blocking wait becomes a non-blocking one. |
| `base::CancelableEvent` | — | A `WaitableEvent` variant supporting cancellation of a pending wait. |

The pattern across the whole table: for nearly every primitive that would park a
thread, `base/` also ships the version that posts a callback instead — and that
is the one the tree expects you to use.

## 9. What `std::` offers, and why almost none of it is used

The C++ standard library has a complete concurrency toolkit. Chromium uses
essentially one piece of it. `styleguide/c++/c++-features.md` bans the rest in a
single entry, **Thread Support Library**, listing eight headers at once:

```c++
#include <barrier>             // C++20
#include <condition_variable>
#include <future>
#include <latch>               // C++20
#include <mutex>
#include <semaphore>           // C++20
#include <stop_token>          // C++20
#include <thread>
```

with a one-line reason: *"Overlaps with `base/synchronization`. `base::Thread` is
tightly coupled to `base::MessageLoop` which would make it hard to replace."*
That is a migration-cost argument, not a claim that the standard versions are
bad — but the effect is a hard wall.

| `std::` | Status | Chromium equivalent | The actual difference |
|---|---|---|---|
| `std::thread`, `std::jthread` | **banned** | `base::Thread` (159), `base::SimpleThread` (26), `base::PlatformThread` (971) | `base::Thread` owns a message loop, so you can *post* to it; a `std::thread` only runs a function. `std::jthread` has 0 uses. |
| `std::mutex`, `std::lock_guard`, `std::unique_lock` | **banned** | `base::Lock` (487), `base::AutoLock` (2,275) | `base::Lock` is a `pthread_mutex` with `ERRORCHECK` and priority inheritance, integrates with Clang's `GUARDED_BY`, and carries the tree's lock-order and metrics instrumentation. |
| `std::shared_mutex` | not in the banned list, but **0 uses** | — | The lexicon states it outright: "Chrome doesn't expose reader-writer locks." The sanctioned alternative is an immutable global initialised once. |
| `std::condition_variable` | **banned** | `base::ConditionVariable` (18) | Both exist; both are avoided. §6 explains why — waiting is restricted at runtime, not just discouraged. |
| `std::future`, `std::promise`, `std::async` | **banned** | `PostTaskAndReplyWithResult` (1,221) | The closest and most instructive pair. Both express "compute elsewhere, get the value back", but a `std::future` is collected by **blocking** on `get()`, while the Chromium version delivers the result as a **callback on your sequence**. That inversion is the whole design. |
| `std::latch`, `std::barrier` | **banned** | `base::BarrierClosure` (302) | Same again: a `std::latch` is waited on; a `BarrierClosure` counts down and then *runs a callback*. Nothing parks. |
| `std::binary_semaphore`, `std::counting_semaphore` | **banned** | `base::WaitableEvent` (1,592) | With `WaitableEventWatcher` to convert the wait into a callback when it must not block. |
| `std::stop_token`, `std::stop_source` | **banned** | `base::WeakPtr`, `base::CancelableTaskTracker` (800) | Chromium ties cancellation to **object lifetime** (a `WeakPtr` cancels the callback when the receiver dies) or to an explicit task id, rather than to a token passed down a call chain. |
| `std::call_once`, `std::once_flag` | from banned `<mutex>` (15 uses each, mostly `base/`, `ui/gfx` and WTF) | function-local `static`, `base::NoDestructor` | Function-local static initialisation has been thread-safe since C++11, which covers nearly every case `call_once` was invented for. |
| `thread_local` | **allowed** (136 uses) | `SequenceLocalStorageSlot` (93) | The language keyword works; it is just usually the wrong *scope*, since work migrates between pool threads. Sequence-local storage is the shape that matches. |
| **`std::atomic`** | **allowed** — **815 uses** | — | The one piece adopted wholesale. `std::atomic_flag` (7) loses to `base::AtomicFlag` (44), which is one-way by construction: set once, never cleared. |

Two observations worth taking away.

**The pattern in the "Chromium equivalent" column is always the same
transformation.** Take a standard primitive whose interface is *"block until"*
and replace it with one whose interface is *"call me when"*. `future::get()`
becomes a reply callback, `latch::wait()` becomes a barrier closure,
`semaphore::acquire()` becomes a watched event. That is not a stylistic
preference: a browser has threads that must never stop pumping their message
loop, so a primitive that blocks by design is unusable on most of them.

**This is the mirror image of the pointer story.** §4 of the
[idioms note](chromium-cpp-idioms.md) found that `std::unique_ptr` survived
untouched because ownership is something the standard library models well. Here
almost nothing survives, because what the standard library models is *threads*,
and Chromium's unit of concurrency is the **sequence** — which has no standard
counterpart at all.

## 10. Blink is not an exception, quite

`third_party/blink/renderer/` overrides many Chromium conventions but **not this
one**: `base::AutoLock` appears 751 times inside Blink, so it uses the same
locks. WTF adds only narrow extras, such as `RecursiveMutex`
([`threading_primitives.h:50`](https://github.com/obeletski/chromium/blob/floating-window/third_party/blink/renderer/platform/wtf/threading_primitives.h#L50)).

What differs is the *assertion idiom*. Blink's dominant check is
`DCHECK(IsMainThread())` — 611 uses — rather than a per-object
`SEQUENCE_CHECKER`, because so much of Blink is main-thread-only by construction
rather than sequence-bound. Oilpan's garbage collector reinforces that: a
`Member<T>` is not a thread-safe handle, and objects are traced per-thread, so
"just post it to another sequence" is not available for GC objects the way it is
for ordinary ones. Cross-thread work in Blink goes through the explicit
`CrossThreadBindOnce` / `CrossThreadHandle` machinery instead — see §10 of the
[idioms note](chromium-cpp-idioms.md).

---

## 11. Where to read more

| Question | Read |
|---|---|
| The canonical guide, at length | [`docs/threading_and_tasks.md`](https://github.com/obeletski/chromium/blob/floating-window/docs/threading_and_tasks.md) |
| "Why can't I just…" | [`docs/threading_and_tasks_faq.md`](https://github.com/obeletski/chromium/blob/floating-window/docs/threading_and_tasks_faq.md) |
| Testing threaded code | [`docs/threading_and_tasks_testing.md`](https://github.com/obeletski/chromium/blob/floating-window/docs/threading_and_tasks_testing.md) |
| Callback and binding semantics | [`docs/callback.md`](https://github.com/obeletski/chromium/blob/floating-window/docs/callback.md) |
| Blocking a thread in a test, correctly | [`docs/patterns/synchronous-runloop.md`](https://github.com/obeletski/chromium/blob/floating-window/docs/patterns/synchronous-runloop.md) |
| The primitives themselves | [`base/synchronization/`](https://github.com/obeletski/chromium/blob/floating-window/base/synchronization/lock.h), [`base/threading/thread_restrictions.h`](https://github.com/obeletski/chromium/blob/floating-window/base/threading/thread_restrictions.h) |
| Which `std::` concurrency features are banned, and why | §2 of [`chromium-cpp-idioms.md`](chromium-cpp-idioms.md) |

---

## 12. What is verified here, and what is not

**Measured in this checkout:** every count in this note, by grepping `*.cc` and
`*.h` under `base/ chrome/ content/ components/ ui/
third_party/blink/renderer/`. The condition-variable figure is a reference
count, not a construction count, and the file list behind it is what supports
the "mostly inside `base/`" claim.

**Read and quoted directly:** the lexicon and lock guidance in
`threading_and_tasks.md`, the blocking categories in `thread_restrictions.h`,
the `pthread_mutex` attributes in `lock_impl_posix.cc`, the "do not add new
threads here" note in `browser_thread.h`, and the absence of any
reader-writer lock.

**Not verified:** that `GUARDED_BY` fires as a compile error in this build
configuration specifically — the annotation is Clang's and the tree enables it,
but no failing build was produced to confirm it. The renderer's thread list in
§4 is also simplified; compositor and worker threads vary by configuration.

**Named but not explored:** everything in §8 is listed with its purpose and a
usage count, not walked through. `WaitableEventWatcher` and `CancelableEvent`
have no count because a reliable one needs disambiguating from their own headers
and tests.

**Out of scope:** the `SequenceManager` and `MessagePump` internals, the full
set of scheduling traits, and the `ThreadPool` shutdown semantics that decide
whether a posted task runs at all.
