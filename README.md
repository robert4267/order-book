# order-book

A single-instrument limit order book and matching engine in C++20. Price-time
priority, limit orders with GTC/IOC/FOK/post-only, market orders, cancels and
modifies. Everything is allocated in the constructor; nothing on the hot path
touches the heap. It is checked against a deliberately simple `std::map`
reference book by a differential fuzz test, and there is a NASDAQ ITCH 5.0
replay tool that rebuilds books from a feed.

Header-only, no dependencies outside the standard library (GoogleTest for the
tests).

- 2.5-5.4x the throughput of a `std::map` book on the same flow, and the gap
  grows as the book gets deeper (see [Results](#results)).
- p99 per-op latency of 266-476 ns, p50 of 35-108 ns, on a shared VM.
- Differentially fuzzed against the reference book (about 1.45M random ops
  over 290 seeds), plus unit and regression tests, run under ASan and UBSan
  in CI.

## Results

Synthetic flow, one instrument, ops pre-generated, single thread.
Xeon @ 2.8 GHz in a 2-vCPU KVM VM (not isolated), g++ 13 `-O3`,
pinned with `taskset -c 1`. Throughput is the median over seeds 42-46;
ranges are across four full runs, and the speedup is per run.

| book depth                        | `OrderBook` Mops/s | `std::map` Mops/s | speedup  |
|-----------------------------------|--------------------|-------------------|----------|
| shallow: ~140 resting, 43 levels  | 26-32              | 10.2-11.1         | 2.5-3.1x |
| medium: ~2.2k resting, 212 levels | 31-34              | 8.3-8.7           | 3.7-4.1x |
| deep: ~20.5k resting, 701 levels  | 26-29              | 4.9-6.0           | 4.7-5.4x |

The gap grows with depth: the `std::map` book's tree and list nodes fall out
of cache as the book gets bigger, while `OrderBook`'s cost per op barely
moves.

Per-operation latency of `OrderBook`, ns:

| book depth | p50    | p99     | p99.9   |
|------------|--------|---------|---------|
| shallow    | 40-89  | 364-433 | 559-606 |
| medium     | 35-90  | 266-426 | 532-589 |
| deep       | 65-108 | 375-476 | 579-711 |

This is a shared VM and the numbers move between runs, single-seed
throughput by as much as 2x. The ratio to `std::map` within a run is more
stable than the absolute figures. Run `lob_bench` on your own machine before
quoting anything.

How these were measured, and what they leave out:

- Each op is timed with `lfence; rdtsc` before and `rdtscp; lfence` after,
  calibrated against `steady_clock`. The smallest empty-region time (about
  11 ns here) is subtracted from every sample. On non-x86 the bench falls back
  to `steady_clock`, whose cost is close to the ops themselves, so trust the
  throughput column there, not the percentiles. Percentiles are the median
  of 3 timed passes, each on a fresh book.
- Throughput runs with no per-op timer: median of 5 passes, each on a fresh
  book, after a warmup pass. The warmup uses a separate book, so it warms code
  and branch predictors but not the measured book's data.
- Latency is closed-loop service time. The next op starts when the previous
  one returns, so queueing under a real arrival rate (coordinated omission)
  is not captured.
- The flow is generated against the reference book, so every cancel and
  modify hits a live order. Mix on the shallow run: 55% limit, 35% cancel,
  5% modify, 5% market, with limit prices clustered near a random-walk mid.
  Real flow is cancel-heavy too, since most resting orders are pulled before
  they trade, but the exact mix here is a guess, not a measurement.
- The medium and deep runs hold a floor of resting orders by turning some
  cancels into adds, so their mix shifts toward adds; the bench prints the
  realized mix.
- Not measured: parsing, networking, multiple instruments, anything
  multi-threaded.
- p50 on this VM is bimodal across process runs (around 35 vs 85 ns on the
  shallow flow) while throughput stays put, which is why ranges are quoted.

`OrderBook` allocates about 11 MB for these runs (orders 4 MB, id table
4 MB, levels 3 MB). The id table is sized from capacity, not from how many
orders are resting, so capacity sets its cache footprint. On this flow a 4x
smaller table gained only about 5%; in the ITCH replay below, where live
orders are old, it mattered far more.

The `std::map` column is the reference book (see Design), with the same
validation as `OrderBook`, including the quantity-overflow check. It keeps a
running total per level so that check is a lookup, not a walk over the
queue; an earlier version walked the queue and made the baseline look
several times slower than it should.

## Design

The baseline is what you'd write first: `std::map<Price, std::list<Order>>`
per side plus an `std::unordered_map` from id to list iterator. Every order is
a heap allocation and every lookup chases pointers. `OrderBook` replaces each
piece:

- **Orders** are 32 bytes in one preallocated array, linked by 32-bit index
  and storing a level index rather than a price, so two fit in a cache line.
  Freed slots go on a free list and are reused first.
- **Price levels** are a flat array indexed by `price - min_price`, each a
  FIFO doubly linked list through the order array. A cancel unlinks its order
  right away, so level totals and `depth_at()` are always exact.
- **Best bid/ask** are cached. When the best level empties, a two-level
  bitmap finds the next one: one bit per level, plus one bit per non-empty
  64-bit word, so a scan moves 4096 levels per step.
- **Order ids** go through an open-addressing table: 16-byte slots, linear
  probing, backward-shift deletion instead of tombstones, Fibonacci hashing,
  load factor at most 0.5 at full capacity. It never rehashes.
- **Trades** go to a listener that's a template parameter, so `on_trade`
  inlines. The book is consistent before each callback, so a throwing
  listener leaves it valid (the rest of the taker's quantity is dropped).

Tradeoffs: the price range and order capacity are fixed at construction.
Memory grows with the tick range (32 bytes per tick for both sides), which
suits one instrument with a known band and is wasteful for a wide one. The
range is capped at 2^28 ticks so level indices fit in 32 bits.

Modify semantics follow most venues: reducing quantity at the same price
keeps queue position; any other change is a cancel/replace and goes to the
back. A full book still accepts a GTC order that would fill completely,
since it never needs a slot.

## Correctness

- **Differential fuzzing.** Random flow goes to `OrderBook` and to the
  `std::map` reference book side by side. After every op the status codes,
  the full trade list, order count and best prices must match; periodically
  (every 50 ops in most scenarios) the test also compares depth at every
  price and every order's remaining quantity, and calls
  `OrderBook::validate()`, which walks the linked lists, level totals, bitmap
  and id table and checks they agree. Five scenarios (narrow band, a wide
  range with far-apart clusters, capacity 6 so the book is constantly full,
  modify-heavy, quantities near 2^64), 290 seeds and about 1.45M ops in
  total. Each scenario also fails if trades, partial fills and
  the rejection it targets (book full, FOK killed, qty overflow) didn't
  actually happen often enough to mean anything.
- **Unit and regression tests** run against both books, so the reference is
  held to the same rules. They include a session worked out by hand, so the
  reference isn't trusted only because the two agree. Bugs found in review
  and pinned by regression tests: signed overflow building a book over the
  full `int64` range, level quantity wrapping past 2^64, FOK's
  available-quantity sum wrapping, and the text driver accepting `-5` as a
  quantity.
- Separate randomized tests for the id table (against `std::unordered_map`,
  including clusters that wrap around the table) and the bitmap (against
  `std::set`, sizes on both sides of the 64 and 4096 boundaries).
- 131 tests. CI builds with gcc and clang on Linux, Apple clang on macOS, and
  gcc with ASan and UBSan, all with `-Werror`, and checks clang-format.

## ITCH 5.0 replay

`lob_itch_replay` decodes a NASDAQ TotalView-ITCH 5.0 file and rebuilds the
book for one symbol (or all of them with `--all`). The decoder reads fields
in place with `memcpy` and allocates nothing. Every decoded message type has
a test fixture written byte by byte from the field offsets in the official
TotalView-ITCH 5.0 spec, independently of the encoder the other tests use.
Adds go in as post-only: a real feed never asks the book to match, so a
crossing add (around halts and auction crosses) is refused and counted
rather than producing trades that never happened. Executions and partial
cancels shrink the order in place, which keeps its queue position.

It has only been run on synthetic files and hand-built fixtures; no real
NASDAQ file was available while writing it. NASDAQ publishes full-day
samples; 12302019 is the smallest at about 3.5 GB gzipped, with a `.md5sum`
next to each file:

```sh
wget 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz'
zcat 12302019.NASDAQ_ITCH50.gz | ./build/lob_itch_replay - AAPL
```

On a synthetic 10M-message file from `lob_make_itch` (6 symbols, ~3000
resting orders per symbol, mostly adds and deletes), decoding alone runs at
about 97M messages/s (10 ns/msg). Maintaining the AAPL book costs 85-90 ns
per book update on top of decoding at the default capacity of 262144
orders, and 31-37 ns at 8192 (`--max-orders 8192`). Ids increase through
the day, so live orders are scattered across the whole id table and nearly
every lookup misses cache; a smaller table misses less. The default is
sized for a busy symbol on a real day, which can hold far more resting
orders than this synthetic file. These numbers are synthetic and not
comparable to results on a real day.

`--all` allocates every book at full capacity as soon as its symbol first
appears, so its defaults are smaller (8192 orders and 16384 levels, about
1 MB per book at most). A real day has several thousand symbols, which still means
several GB. Adds that don't fit are counted as "full" or "out of range"
rather than silently dropped.

## Limitations

- One instrument per book, fixed tick range and capacity.
- No self-trade prevention, no owner or account concept, no icebergs or stops.
- No market data output beyond the trade callback and `for_each_level()`.
- Single-threaded; there is no gateway, sequencer or persistence.
- Reducing an order (ITCH E/X) takes a lookup and then a modify, two probes
  where a dedicated reduce call would take one.

## Related work

The [QuantCup 1 winner](https://gist.github.com/druska/d6ce3f2bac74db08ee9007cdf98106ef)
uses the same flat price array, but the engine assigns order ids (so they
index the order arena directly, with no hash table), cancels lazily by
zeroing size, and finds the next price by linear scan. This book takes
arbitrary client ids, unlinks eagerly so depth is exact, and uses the bitmap.

[CppTrader](https://github.com/chronoxor/CppTrader) replays a real NASDAQ
day (283M messages) on an i7-4790K and reports 24 ns/msg for parsing and
102-309 ns/msg for building every symbol's book.

[exchange-core](https://github.com/exchange-core/exchange-core) benchmarks
open-loop at fixed arrival rates through a full exchange pipeline with risk
checks, so its latencies aren't comparable to the closed-loop, book-only
numbers here.

## Build

CMake 3.21+ and a C++20 compiler. GoogleTest is fetched at configure time,
pinned to the v1.15.2 commit.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
taskset -c 1 ./build/lob_bench          # lob_bench [ops] [seed] [seeds]
```

`-DLOB_SANITIZE=ON` adds ASan and UBSan, `-DLOB_NATIVE=ON` adds
`-march=native`, `-DLOB_WERROR=ON` is what CI uses. Used through
`add_subdirectory`, only the `lob::lob` header target is defined.

## Usage

```cpp
#include "lob/order_book.hpp"

struct Fills {
    lob::Qty traded = 0;
    void on_trade(const lob::Trade& t) { traded += t.qty; }  // also t.taker, t.maker, t.price
};

lob::OrderBook<Fills> book(/*min_price*/ 0, /*max_price*/ 100'000, /*max_orders*/ 1 << 18);
using lob::Side, lob::Status, lob::Tif;

if (book.add(1, Side::Sell, 10'050, 100) != Status::Ok) { /* rejected */ }
(void)book.add(2, Side::Buy, 10'050, 40);  // trades 40 against order 1

Status s = book.add(3, Side::Buy, 10'050, 50, Tif::PostOnly);
if (s == Status::WouldCross) { /* would have traded with order 1 */ }

s = book.modify(1, 10'050, 30);  // smaller at the same price, keeps priority
s = book.cancel(1);
```

`Status` is `[[nodiscard]]`, so ignoring a rejection is a warning.

There's also a text driver (`A id side price qty [GTC|IOC|FOK|POST]`, `M`
market, `X` cancel, `R` modify, `P` print; see `tools/replay.cpp`):

```sh
$ printf 'A 1 S 100 10\nA 2 B 99 7\nA 3 B 100 4\nP\n' | ./build/lob_replay
TRADE taker=3 maker=1 price=100 qty=4
   bid qty    price  ask qty
                100  6
         7       99
```

## Layout

```
include/lob/order_book.hpp      the book
include/lob/map_order_book.hpp  std::map reference book and baseline
include/lob/order_id_map.hpp    open-addressing id table
include/lob/price_bitmap.hpp    two-level bitmap over price levels
include/lob/itch.hpp            ITCH 5.0 decoder (and an encoder for tests)
include/lob/itch_book.hpp       builds books from a decoded ITCH stream
tests/                          unit, regression, fuzz, id table, bitmap, ITCH
bench/                          flow generator and benchmark
tools/                          text driver, ITCH replay, synthetic ITCH writer
```

## License

MIT
