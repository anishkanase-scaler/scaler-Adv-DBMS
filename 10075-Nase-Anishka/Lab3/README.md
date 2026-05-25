# LAB 3 — CLOCK SWEEP BUFFER REPLACEMENT

> Roll Number: **10075** &nbsp;&nbsp;|&nbsp;&nbsp; Name: **Nase Anishka**
>
> In this lab I implemented the **Clock Sweep** page replacement policy in
> C++17 — the same algorithm Postgres uses inside `shared_buffers` to
> decide which buffer to throw out when a new page needs to be brought in.
> I wanted to actually feel why it's called "second chance" instead of
> just reading about it, so I built a small templated cache and traced
> exactly which frame gets evicted at every step.

---

# WHAT IS CLOCK SWEEP

A buffer pool of fixed size needs a victim-selection policy. True LRU
needs a doubly linked list updated on every hit and is expensive under
contention. Clock Sweep is a cheap approximation:

* Every frame has one extra bit — the **reference bit**.
* All frames sit in a **circular array**, and a global **clock hand**
  points to one of them at any moment.
* On a **hit**, set the frame's reference bit to 1.
* On a **miss + cache full**, walk the hand forward:
  * If the bit is **1**, clear it to 0 and advance (this is the "second
    chance" — the page survives one round).
  * If the bit is **0**, **evict** that frame, load the new page there,
    and advance the hand one past the freshly loaded frame.

The trick is that frequently-touched pages keep getting their bit set
back to 1 before the hand returns, so they almost never get picked as
victims — without paying for an LRU list.

---

# FILES IN THIS FOLDER

* `main.cpp` — the `ClockSweepCache<Key, Value>` template plus a driver
  in `main()` that walks six scenarios end-to-end.
* `CMakeLists.txt` — C++17 build, `-Wall -Wextra -Wpedantic`.
* `.gitignore` — build artefacts.
* `run_output.txt` — captured stdout from `./build/clock_sweep` so the
  trace is visible without rebuilding.

---

# BUILD AND RUN

```bash
cd 10075-Nase-Anishka/Lab3
cmake -B build -S .
cmake --build build
./build/clock_sweep
```

The driver prints a verbose trace and dumps the cache state after every
phase. Each dump shows `frame, valid, ref, key, value, hand` so the
clock hand and the reference bits are visible.

---

# THE SCENARIOS

The driver uses a 4-frame cache and walks through:

| # | What happens                                | Key thing it demonstrates                         |
|---|---------------------------------------------|---------------------------------------------------|
| 1 | Insert 4 keys (101..104)                    | No eviction yet — frames fill in order, all ref=1 |
| 2 | `get(101)` and `get(103)`                   | Hits keep ref=1 (they're already 1 from the load) |
| 3 | Insert key 105 → cache is full              | First real sweep: hand walks 0→1→2→3 clearing every ref=1 to 0, wraps to 0 which is now 0, evicts 101 |
| 4 | Insert key 106                              | Hand is already on a cold frame (102, ref=0) — direct eviction, no sweep walk |
| 5 | `put(105, "erin-updated")`                  | Existing key: no eviction, just update + ref=1    |
| 6 | `get(101)` after it was evicted             | Returns `std::nullopt` — proves it really is gone |

---

# SAMPLE OUTPUT

```text
Clock-Sweep buffer cache demo (capacity = 4)
  LOAD  put(101, alice)   [frame 0]
  LOAD  put(102, bob)     [frame 1]
  LOAD  put(103, carol)   [frame 2]
  LOAD  put(104, dave)    [frame 3]

--- after initial 4 inserts ---
frame  valid  ref  key   value   hand
0      yes    1    101   alice   <--
1      yes    1    102   bob
2      yes    1    103   carol
3      yes    1    104   dave

  HIT   get(101) -> alice   [frame 0]
  HIT   get(103) -> carol   [frame 2]

  SWEEP frame 0 ref 1 -> 0
  SWEEP frame 1 ref 1 -> 0
  SWEEP frame 2 ref 1 -> 0
  SWEEP frame 3 ref 1 -> 0
  EVICT frame 0 -> drop key 101
  LOAD  put(105, erin)   [frame 0]

--- after inserting key 105 (one eviction expected) ---
frame  valid  ref  key   value   hand
0      yes    1    105   erin
1      yes    0    102   bob     <--
2      yes    0    103   carol
3      yes    0    104   dave
```

(see `run_output.txt` for the full trace.)

---

# THINGS I LEARNED FROM THIS LAB

* The **reference bit per frame is the entire policy** — there's no
  recency timestamp, no LRU list, no priority queue. Just one bit.
* A frame that was touched between two passes of the clock hand gets a
  "second chance" because the hand will clear its bit to 0 before
  considering eviction; if it's touched again before the hand returns,
  the bit goes back to 1 and it survives indefinitely.
* In the **worst case** (everything has ref=1) the sweep does **at most
  one full lap** clearing bits, then a **second pass** to actually
  evict. So the amortised cost is `O(1)` even though a single
  insertion can do `O(capacity)` work.
* Clock Sweep is fundamentally an **approximation of LRU**, not LRU.
  Two pages that haven't been touched since the last sweep look
  identical — the hand picks the one it reaches first, not the actually
  oldest one. This is the trade-off for keeping the policy O(1) and
  lock-light.
* Real PostgreSQL adds two extras on top of this base algorithm:
  * A **usage count** (small integer, not a single bit) so frequently
    accessed pages survive multiple sweeps.
  * A separate **bgwriter** background process that does the sweeping
    so foreground queries don't pay the latency.
  My version is intentionally just the bit, to keep the core idea
  unobscured.
* Templates were a natural fit — the cache shouldn't care whether the
  keys are `int`, `std::string`, or anything else hashable. I used
  `std::optional<Value>` as the return type of `get` because that's the
  honest signal for "key is not in the cache."

---

# WHAT I'D ADD NEXT

* A real **usage counter** (0..3) instead of a single bit, matching
  PostgreSQL's actual `BufferDesc.usage_count`.
* Pinning — buffers being actively read shouldn't be evictable even if
  their ref bit is 0.
* A background thread that runs the sweep proactively instead of only
  on demand, so misses don't pay the full lap cost.
