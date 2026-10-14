// Replays the same synthetic order flow through both books and reports
// throughput and per-operation latency, for three book depths.
//
//   lob_bench [ops per scenario, default 2000000] [seed, default 42] [seeds, default 5]
//
// The whole op sequence is generated up front, so no number includes parsing,
// I/O or the generator. Single thread, one instrument, no pinning: run it
// under `taskset -c N` on a quiet machine if you want stable tails.
//
// Latency is closed-loop service time: the next op starts when the previous
// one returns, so queueing delay under a real arrival process (coordinated
// omission) is not captured.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "flow.hpp"
#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define LOB_HAVE_RDTSC 1
#elif defined(_M_X64)
#include <intrin.h>
#define LOB_HAVE_RDTSC 1
#endif

using namespace lob;
using namespace lob::bench;
using Clock = std::chrono::steady_clock;

namespace {

#ifdef LOB_HAVE_RDTSC
// The fences keep the timed op from leaking outside the two reads (Paoloni,
// "How to Benchmark Code Execution Times on Intel IA-32 and IA-64").
inline std::uint64_t start_ticks() {
    _mm_lfence();
    return __rdtsc();
}
inline std::uint64_t stop_ticks() {
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}
#else
inline std::uint64_t start_ticks() {
    return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
}
inline std::uint64_t stop_ticks() { return start_ticks(); }
#endif

// Ticks are converted with a ratio measured against steady_clock, which also
// makes the fallback path work without knowing the clock's period.
double ns_per_tick() {
    const auto c0 = Clock::now();
    const std::uint64_t t0 = start_ticks();
    while (Clock::now() - c0 < std::chrono::milliseconds(200)) {
    }
    const auto c1 = Clock::now();
    const std::uint64_t t1 = stop_ticks();
    const double ns = std::chrono::duration<double, std::nano>(c1 - c0).count();
    return ns / static_cast<double>(t1 - t0);
}

// Checksums trades so the work isn't optimized away and both books can be
// compared.
struct Checksum {
    std::uint64_t trades = 0;
    std::uint64_t hash = 0;
    void on_trade(const Trade& t) {
        ++trades;
        hash = hash * 31 + t.maker * 7 + static_cast<std::uint64_t>(t.price) * 13 + t.qty;
    }
};

// Fits the deep scenario (~25k resting at peak). Much bigger and the id table
// falls out of cache.
constexpr std::size_t kCapacity = 1 << 17;

void print_footprint(const FlowConfig& cfg) {
    using Book = OrderBook<>;
    const auto levels = static_cast<std::size_t>(cfg.max_price - cfg.min_price + 1);
    const double mb = 1024.0 * 1024.0;
    auto size_mb = [&](std::size_t n, std::size_t bytes) {
        return static_cast<double>(n) * static_cast<double>(bytes) / mb;
    };
    std::printf(
        "OrderBook allocation: orders %.1f MB, id map %.1f MB, levels %.1f MB (%zu B/order)\n",
        size_mb(kCapacity, Book::order_bytes()),
        size_mb(OrderIdMap::capacity_for(kCapacity), OrderIdMap::slot_bytes()),
        size_mb(2 * levels, Book::level_bytes()), Book::order_bytes());
    std::printf(
        "the id map is sized from capacity, not from resting orders, so capacity sets its\n"
        "cache footprint\n");
}

template <class Book>
inline Status apply(Book& book, const Op& op) {
    switch (op.type) {
        case OpType::Add: return book.add(op.id, op.side, op.price, op.qty);
        case OpType::Market: return book.market(op.id, op.side, op.qty);
        case OpType::Cancel: return book.cancel(op.id);
        case OpType::Modify: return book.modify(op.id, op.price, op.qty);
    }
    return Status::Ok;
}

constexpr std::size_t kStatuses = kStatusCount;
constexpr std::size_t kTypes = 4;
constexpr std::array<const char*, kTypes> kTypeNames{"add", "market", "cancel", "modify"};

struct Pass {
    double mops = 0;
    std::uint64_t trades = 0, hash = 0;
    // [op type][status], to check cancels and modifies really hit live orders
    std::array<std::array<std::size_t, kStatuses>, kTypes> status{};
};

template <template <class> class BookT>
Pass timed_pass(const std::vector<Op>& ops, const FlowConfig& cfg) {
    Pass p;
    BookT<Checksum> book(cfg.min_price, cfg.max_price, kCapacity);
    const auto t0 = Clock::now();
    for (const Op& op : ops) {
        ++p.status[static_cast<std::size_t>(op.type)][static_cast<std::size_t>(apply(book, op))];
    }
    const auto t1 = Clock::now();
    p.mops = static_cast<double>(ops.size()) / std::chrono::duration<double>(t1 - t0).count() / 1e6;
    p.trades = book.listener().trades;
    p.hash = book.listener().hash;
    return p;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

struct Throughput {
    double median = 0, lo = 0, hi = 0;
    Pass last;
};

// One untimed warmup pass, then `reps` timed passes, each on a fresh book.
template <template <class> class BookT>
Throughput throughput(const std::vector<Op>& ops, const FlowConfig& cfg, int reps) {
    Throughput t;
    t.last = timed_pass<BookT>(ops, cfg);
    std::vector<double> mops;
    for (int r = 0; r < reps; ++r) {
        t.last = timed_pass<BookT>(ops, cfg);
        mops.push_back(t.last.mops);
    }
    t.median = median(mops);
    t.lo = *std::min_element(mops.begin(), mops.end());
    t.hi = *std::max_element(mops.begin(), mops.end());
    return t;
}

struct Percentiles {
    double p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

Percentiles percentiles(std::vector<std::uint64_t>& v, double scale) {
    Percentiles p;
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) {
        return static_cast<double>(
                   v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))]) *
               scale;
    };
    p.p50 = at(0.50);
    p.p90 = at(0.90);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    p.max = static_cast<double>(v.back()) * scale;
    return p;
}

struct Latency {
    Percentiles all;
    std::array<Percentiles, kTypes> by_type;
    std::size_t clamped = 0;  // samples no slower than an empty timed region
};

// Times every op on its own. The fastest empty timed region is subtracted
// from each sample: that never removes more than the timer really cost, so
// the result is still an upper bound on the op itself.
template <template <class> class BookT>
Latency latency_pass(const std::vector<Op>& ops, const FlowConfig& cfg, double scale,
                     std::uint64_t overhead) {
    std::vector<std::uint64_t> dt(ops.size());
    BookT<Checksum> book(cfg.min_price, cfg.max_price, kCapacity);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const std::uint64_t t0 = start_ticks();
        static_cast<void>(apply(book, ops[i]));
        const std::uint64_t t1 = stop_ticks();
        dt[i] = t1 - t0 > overhead ? t1 - t0 - overhead : 0;
    }

    Latency l;
    l.clamped = static_cast<std::size_t>(std::count(dt.begin(), dt.end(), 0));
    std::array<std::vector<std::uint64_t>, kTypes> split;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        split[static_cast<std::size_t>(ops[i].type)].push_back(dt[i]);
    }
    for (std::size_t k = 0; k < kTypes; ++k) l.by_type[k] = percentiles(split[k], scale);
    l.all = percentiles(dt, scale);
    return l;
}

Percentiles median_of(const std::vector<Percentiles>& v) {
    auto field = [&](double Percentiles::*f) {
        std::vector<double> x;
        for (const Percentiles& p : v) x.push_back(p.*f);
        return median(x);
    };
    return {field(&Percentiles::p50), field(&Percentiles::p90), field(&Percentiles::p99),
            field(&Percentiles::p999), field(&Percentiles::max)};
}

// A warmup pass, then kLatencyReps timed passes on fresh books; each
// percentile is the median across passes. That steadies the tails but not
// run-to-run differences in p50, so compare a few runs.
constexpr int kLatencyReps = 3;

template <template <class> class BookT>
Latency latency(const std::vector<Op>& ops, const FlowConfig& cfg, double scale,
                std::uint64_t overhead) {
    {
        BookT<Checksum> warm(cfg.min_price, cfg.max_price, kCapacity);
        for (const Op& op : ops) static_cast<void>(apply(warm, op));
    }
    std::vector<Latency> runs;
    for (int r = 0; r < kLatencyReps; ++r) {
        runs.push_back(latency_pass<BookT>(ops, cfg, scale, overhead));
    }
    Latency l;
    std::vector<Percentiles> all;
    for (const Latency& r : runs) {
        all.push_back(r.all);
        l.clamped = std::max(l.clamped, r.clamped);
    }
    l.all = median_of(all);
    for (std::size_t k = 0; k < kTypes; ++k) {
        std::vector<Percentiles> t;
        for (const Latency& r : runs) t.push_back(r.by_type[k]);
        l.by_type[k] = median_of(t);
    }
    return l;
}

struct Overhead {
    std::uint64_t min_ticks = 0;
    double min_ns = 0, p50_ns = 0, p99_ns = 0;
};

Overhead timer_overhead(double scale) {
    constexpr int kSamples = 1'000'000;
    std::vector<std::uint64_t> dt(kSamples);
    for (auto& x : dt) {
        const std::uint64_t t0 = start_ticks();
        const std::uint64_t t1 = stop_ticks();
        x = t1 - t0;
    }
    Overhead o;
    o.min_ticks = *std::min_element(dt.begin(), dt.end());
    o.min_ns = static_cast<double>(o.min_ticks) * scale;
    const Percentiles p = percentiles(dt, scale);
    o.p50_ns = p.p50;
    o.p99_ns = p.p99;
    return o;
}

#ifdef LOB_HAVE_RDTSC
bool cpu_flag(const char* flag) {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("flags", 0) == 0)
            return line.find(std::string(" ") + flag) != std::string::npos;
    }
    return false;
}
#endif

double hit_rate(const Pass& p, OpType t) {
    const auto& s = p.status[static_cast<std::size_t>(t)];
    std::size_t total = 0;
    for (std::size_t n : s) total += n;
    return total ? 100.0 * static_cast<double>(s[static_cast<std::size_t>(Status::Ok)]) /
                       static_cast<double>(total)
                 : 0;
}

void print_row(const char* name, const Throughput& t, const Percentiles& l) {
    std::printf("%-10s %7.2f [%5.2f-%5.2f] %7.0f %7.0f %7.0f %8.0f %10.0f\n", name, t.median, t.lo,
                t.hi, l.p50, l.p90, l.p99, l.p999, l.max);
}

// Returns false if the two books disagree or the book filled up.
bool run_scenario(const char* name, const FlowConfig& cfg, int seeds, double scale,
                  std::uint64_t overhead) {
    constexpr int kReps = 5;
    std::printf("== %s: %zu ops, seed %llu ==\n", name, cfg.ops,
                static_cast<unsigned long long>(cfg.seed));
    const Flow flow = make_flow(cfg);
    const auto& ops = flow.ops;
    const FlowStats& fs = flow.stats;

    std::array<std::size_t, kTypes> counts{};
    for (const Op& op : ops) ++counts[static_cast<std::size_t>(op.type)];
    const auto share = [&](std::size_t k) {
        return 100.0 * static_cast<double>(counts[k]) / static_cast<double>(ops.size());
    };
    std::printf("mix: %.1f%% add, %.1f%% cancel, %.1f%% modify, %.1f%% market\n", share(0),
                share(2), share(3), share(1));
    std::printf(
        "book: %.0f resting orders avg (max %zu), %.0f non-empty levels avg, %zu distinct prices "
        "used,\n      spread %.1f ticks avg, %.1f%% of adds trade on arrival\n",
        fs.resting_avg, fs.resting_max, fs.levels_avg, fs.levels_touched, fs.spread_avg,
        100.0 * fs.adds_trading);

    const Throughput base = throughput<MapOrderBook>(ops, cfg, kReps);
    const Throughput fast = throughput<OrderBook>(ops, cfg, kReps);
    const Latency base_lat = latency<MapOrderBook>(ops, cfg, scale, overhead);
    const Latency fast_lat = latency<OrderBook>(ops, cfg, scale, overhead);

    std::printf("cancel hit %.1f%%, modify hit %.1f%%, %llu trades\n\n",
                hit_rate(fast.last, OpType::Cancel), hit_rate(fast.last, OpType::Modify),
                static_cast<unsigned long long>(fast.last.trades));
    std::printf("%-10s %7s %13s %7s %7s %7s %8s %10s\n", "book", "Mops/s", "[min-max]", "p50 ns",
                "p90", "p99", "p99.9", "max");
    print_row("std::map", base, base_lat.all);
    print_row("OrderBook", fast, fast_lat.all);
    std::printf(
        "samples at 0 after overhead subtraction (worst pass): std::map %.2f%%, OrderBook %.2f%%\n",
        100.0 * static_cast<double>(base_lat.clamped) / static_cast<double>(ops.size()),
        100.0 * static_cast<double>(fast_lat.clamped) / static_cast<double>(ops.size()));
    std::printf("OrderBook: mean %.1f ns/op from throughput (no per-op timer), %.1fx std::map\n",
                1e3 / fast.median, fast.median / base.median);

    std::printf("\nOrderBook latency by op type, ns\n%-10s %7s %7s %7s %8s\n", "op", "p50", "p90",
                "p99", "p99.9");
    for (std::size_t k = 0; k < kTypes; ++k) {
        const Percentiles& p = fast_lat.by_type[k];
        std::printf("%-10s %7.0f %7.0f %7.0f %8.0f\n", kTypeNames[k], p.p50, p.p90, p.p99, p.p999);
    }

    bool ok = true;
    const std::size_t full = fast.last.status[0][static_cast<std::size_t>(Status::BookFull)];
    if (full > 0) {
        std::printf("warning: %zu orders rejected as book full, raise kCapacity\n", full);
        ok = false;
    }
    if (base.last.trades != fast.last.trades || base.last.hash != fast.last.hash) {
        std::printf("MISMATCH: the two books produced different trades\n");
        ok = false;
    }

    if (seeds > 1) {
        std::printf("\nthroughput across seeds, median of 3 passes after a warmup, Mops/s\n");
        std::vector<double> all_fast, all_base;
        for (int s = 0; s < seeds; ++s) {
            FlowConfig c = cfg;
            c.seed = cfg.seed + static_cast<std::uint64_t>(s);
            const Flow f = make_flow(c);
            all_base.push_back(throughput<MapOrderBook>(f.ops, c, 3).median);
            all_fast.push_back(throughput<OrderBook>(f.ops, c, 3).median);
        }
        auto spread = [](const char* label, const std::vector<double>& v) {
            std::printf("%-10s median %6.2f  min %6.2f  max %6.2f  (", label, median(v),
                        *std::min_element(v.begin(), v.end()),
                        *std::max_element(v.begin(), v.end()));
            for (double x : v) std::printf(" %.1f", x);
            std::printf(" )\n");
        };
        std::printf(
            "seeds %llu-%llu\n", static_cast<unsigned long long>(cfg.seed),
            static_cast<unsigned long long>(cfg.seed + static_cast<std::uint64_t>(seeds) - 1));
        spread("std::map", all_base);
        spread("OrderBook", all_fast);
    }
    std::printf("\n");
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    FlowConfig cfg;
    int seeds = 5;
    if (argc > 1) cfg.ops = std::stoull(argv[1]);
    if (argc > 2) cfg.seed = std::stoull(argv[2]);
    if (argc > 3) seeds = std::max(1, std::stoi(argv[3]));

    const double scale = ns_per_tick();
    const Overhead ovh = timer_overhead(scale);
    std::printf("ops are pre-generated against the std::map book; timing covers the book only\n");
    std::printf("throughput: median of 5 passes after a warmup pass, each on a fresh book\n");
    std::printf("warmup runs on its own book: it warms code and branch predictors, not the data\n");
#ifdef LOB_HAVE_RDTSC
    std::printf("latency: every op timed with lfence+rdtsc / rdtscp+lfence, %.3f GHz TSC%s\n",
                1.0 / scale,
                cpu_flag("constant_tsc") && cpu_flag("nonstop_tsc")
                    ? " (constant_tsc, nonstop_tsc)"
                    : ", WARNING: TSC may not be invariant on this CPU");
#else
    std::printf(
        "latency: every op timed with steady_clock, %.1f ns per tick; the clock read costs about "
        "as much as an op here, so trust throughput over the percentiles\n",
        scale);
#endif
    std::printf(
        "timer overhead: min %.1f ns (subtracted from every sample), p50 %.1f, p99 %.1f\n"
        "median of %d passes after a warmup pass, %zu samples each; closed-loop service time\n",
        ovh.min_ns, ovh.p50_ns, ovh.p99_ns, kLatencyReps, cfg.ops);
    print_footprint(cfg);
    std::printf("\n");

    FlowConfig mid = cfg;
    mid.min_resting = 2'000;
    mid.distance_p = 0.05;
    FlowConfig deep = cfg;
    deep.min_resting = 20'000;
    deep.distance_p = 0.02;

    bool ok = run_scenario("default flow, shallow book", cfg, seeds, scale, ovh.min_ticks);
    ok = run_scenario("medium book (>= 2k resting)", mid, seeds, scale, ovh.min_ticks) && ok;
    ok = run_scenario("cache stress (>= 20k resting)", deep, seeds, scale, ovh.min_ticks) && ok;
    return ok ? 0 : 1;
}
