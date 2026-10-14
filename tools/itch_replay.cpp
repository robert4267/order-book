// Replays a NASDAQ TotalView-ITCH 5.0 file (uncompressed, 2-byte length
// framing as in NASDAQ's sample files) and rebuilds order books from it.
//
//   lob_itch_replay FILE [SYMBOL] [options]
//   zcat 12302019.NASDAQ_ITCH50.gz | lob_itch_replay - AAPL --no-baseline
//
//   SYMBOL           book to build, default AAPL
//   --all            build a book for every symbol instead
//   --parse-only     decode every message, build nothing
//   --tick N         ITCH price units per book tick (default 1 = $0.0001,
//                    100 with --all)
//   --window F       book covers first price * (1 +/- F), default 0.5
//   --centre P       centre the window on P (ITCH units) instead of the first
//                    price, for symbols whose first order is far off the market
//   --max-levels N   cap on levels per book (default 4194304, 65536 with --all)
//   --max-orders N   resting orders per book (default 262144, 65536 with --all)
//   --depth N        levels to print per side (default 5)
//   --no-baseline    skip the parse-only pass used to isolate book cost
//
// Timing covers decoding and book updates only, not reading the file.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lob/itch.hpp"
#include "lob/itch_book.hpp"

namespace {

using namespace lob;
using Clock = std::chrono::steady_clock;

struct Run {
    std::uint64_t bytes = 0;
    std::uint64_t messages = 0;
    double seconds = 0;
    std::array<std::uint64_t, 256> types{};
    bool ok = true;
};

// Counts every frame by type and forwards decoded messages to the inner handler.
template <class Inner>
struct Counting {
    Inner& inner;
    std::array<std::uint64_t, 256>& types;

    void on_message(char t) { ++types[static_cast<unsigned char>(t)]; }
    template <class M>
        requires requires(Inner& i, const M& m) { i.on(m); }
    void on(const M& m) {
        inner.on(m);
    }
};

// Sums a few fields per message so the decoding isn't optimized away.
struct Sink {
    std::uint64_t sum = 0;
    void on(const itch::SystemEvent& m) { sum += static_cast<unsigned char>(m.event); }
    void on(const itch::StockDirectory& m) { sum += m.h.locate + m.round_lot; }
    void on(const itch::AddOrder& m) {
        sum += m.h.timestamp + m.ref + m.shares + m.price + static_cast<unsigned char>(m.side) +
               static_cast<unsigned char>(m.stock[0]);
    }
    void on(const itch::OrderExecuted& m) { sum += m.ref + m.shares + m.match + m.price; }
    void on(const itch::OrderCancel& m) { sum += m.ref + m.shares; }
    void on(const itch::OrderDelete& m) { sum += m.ref; }
    void on(const itch::OrderReplace& m) { sum += m.old_ref + m.new_ref + m.shares + m.price; }
};

template <class H>
Run replay(const char* path, H& handler) {
    const bool from_stdin = std::string_view(path) == "-";
    auto close = [from_stdin](std::FILE* fp) {
        if (!from_stdin) std::fclose(fp);
    };
    std::unique_ptr<std::FILE, decltype(close)> f(from_stdin ? stdin : std::fopen(path, "rb"),
                                                  close);
    if (!f) throw std::runtime_error(std::string("can't open ") + path);

    Run run;
    Counting<H> counting{handler, run.types};
    std::vector<char> buf(8u << 20);
    std::size_t have = 0;
    Clock::duration busy{};
    for (;;) {
        const std::size_t got = std::fread(buf.data() + have, 1, buf.size() - have, f.get());
        if (got == 0 && std::ferror(f.get())) {
            std::fprintf(stderr, "read error after %llu bytes\n",
                         static_cast<unsigned long long>(run.bytes));
            run.ok = false;
            break;
        }
        have += got;
        run.bytes += got;
        if (have == 0) break;

        const auto t0 = Clock::now();
        const itch::Result r = itch::parse(buf.data(), have, counting);
        busy += Clock::now() - t0;
        run.messages += r.messages;

        if (r.error == itch::Error::BadLength) {
            std::fprintf(stderr, "malformed frame at byte %llu\n",
                         static_cast<unsigned long long>(run.bytes - have + r.consumed));
            run.ok = false;
            break;
        }
        // keep the partial frame at the end for the next read
        std::memmove(buf.data(), buf.data() + r.consumed, have - r.consumed);
        have -= r.consumed;
        if (got == 0) {
            if (have > 0) {
                std::fprintf(stderr, "file ends inside a frame (%zu trailing bytes)\n", have);
                run.ok = false;
            }
            break;
        }
    }
    run.seconds = std::chrono::duration<double>(busy).count();
    return run;
}

std::string dollars(std::uint64_t itch_price) {
    char out[32];
    std::snprintf(out, sizeof out, "%llu.%04llu",
                  static_cast<unsigned long long>(itch_price / 10000),
                  static_cast<unsigned long long>(itch_price % 10000));
    return out;
}

void print_run(const char* label, const Run& r) {
    if (r.messages == 0 || r.seconds <= 0) {
        std::printf("%-12s no messages\n", label);
        return;
    }
    const double mps = static_cast<double>(r.messages) / r.seconds;
    std::printf("%-12s %llu msgs in %.3f s: %.1f M msgs/s, %.1f ns/msg, %.0f MB/s\n", label,
                static_cast<unsigned long long>(r.messages), r.seconds, mps / 1e6, 1e9 / mps,
                static_cast<double>(r.bytes) / r.seconds / 1e6);
}

void print_types(const Run& r) {
    std::printf("message types:");
    int col = 0;
    for (std::size_t t = 0; t < r.types.size(); ++t) {
        if (r.types[t] == 0) continue;
        if (col++ % 6 == 0) std::printf("\n ");
        std::printf("  %c %-11llu", static_cast<char>(t),
                    static_cast<unsigned long long>(r.types[t]));
    }
    std::printf("\n");
}

void print_stats(const itch::BookStats& s) {
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    std::printf("book messages: add %llu  exec %llu  cancel %llu  delete %llu  replace %llu\n",
                u(s.adds), u(s.executes), u(s.cancels), u(s.deletes), u(s.replaces));
    std::printf(
        "skipped adds: out of range %llu  off tick %llu  crossed %llu  duplicate %llu  "
        "full %llu  bad qty %llu\n",
        u(s.out_of_range), u(s.off_tick), u(s.crossed), u(s.duplicate), u(s.full), u(s.bad_qty));
    std::printf("unknown refs %llu  over-reduce %llu  book updates %llu\n", u(s.unknown_ref),
                u(s.over_reduce), u(s.book_ops));
}

void print_book(const itch::BookBuilder& bb, std::uint16_t locate, std::size_t depth) {
    const auto* book = bb.book(locate);
    if (book == nullptr) {
        std::printf("no book for locate %u\n", locate);
        return;
    }
    std::printf("%.*s: %zu resting orders, range $%s - $%s\n",
                static_cast<int>(bb.name(locate).size()), bb.name(locate).data(),
                book->order_count(), dollars(bb.to_itch(book->min_price())).c_str(),
                dollars(bb.to_itch(book->max_price())).c_str());
    std::vector<std::pair<Price, Qty>> bids, asks;
    book->for_each_level(Side::Buy, [&](Price p, Qty q) { bids.emplace_back(p, q); }, depth);
    book->for_each_level(Side::Sell, [&](Price p, Qty q) { asks.emplace_back(p, q); }, depth);
    std::printf("  %10s %12s | %-12s %10s\n", "bid qty", "bid", "ask", "ask qty");
    for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
        std::string b = i < bids.size() ? dollars(bb.to_itch(bids[i].first)) : "";
        std::string a = i < asks.size() ? dollars(bb.to_itch(asks[i].first)) : "";
        std::string bq = i < bids.size() ? std::to_string(bids[i].second) : "";
        std::string aq = i < asks.size() ? std::to_string(asks[i].second) : "";
        std::printf("  %10s %12s | %-12s %10s\n", bq.c_str(), b.c_str(), a.c_str(), aq.c_str());
    }
}

double fraction(std::string_view s) {
    const std::string str(s);
    char* end = nullptr;
    const double v = std::strtod(str.c_str(), &end);
    if (end != str.c_str() + str.size() || !(v > 0))
        throw std::invalid_argument("bad number: " + str);
    return v;
}

template <class T>
T number(std::string_view s) {
    T v{};
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size() || v <= T{})
        throw std::invalid_argument("bad number: " + std::string(s));
    return v;
}

}  // namespace

int main(int argc, char** argv) try {
    const char* path = nullptr;
    std::string symbol = "AAPL";
    bool all = false, parse_only = false, baseline = true;
    std::size_t depth = 5;
    itch::BookConfig cfg;
    bool tick_set = false, levels_set = false, orders_set = false;

    auto usage = [&](std::FILE* out) {
        std::fprintf(out,
                     "usage: %s FILE [SYMBOL] [--all|--parse-only] [--tick N] "
                     "[--window F] [--centre P] [--max-levels N] [--max-orders N] [--depth N] "
                     "[--no-baseline]\n"
                     "FILE is an uncompressed ITCH 5.0 file, or - for stdin\n",
                     argv[0]);
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(stdout);
            return 0;
        }
        auto value = [&]() -> std::string_view {
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + std::string(a));
            return argv[++i];
        };
        if (a == "--all") all = true;
        else if (a == "--parse-only") parse_only = true;
        else if (a == "--no-baseline") baseline = false;
        else if (a == "--tick") {
            cfg.tick = number<std::uint32_t>(value());
            tick_set = true;
        } else if (a == "--centre") cfg.centre = number<std::uint32_t>(value());
        else if (a == "--window") cfg.window = fraction(value());
        else if (a == "--max-levels") {
            cfg.max_levels = number<std::size_t>(value());
            levels_set = true;
        } else if (a == "--max-orders") {
            cfg.max_orders = number<std::size_t>(value());
            orders_set = true;
        } else if (a == "--depth") depth = number<std::size_t>(value());
        else if (a.starts_with("--"))
            throw std::invalid_argument("unknown option " + std::string(a));
        else if (!path) path = argv[i];
        else symbol = a;
    }
    if (!path) {
        usage(stderr);
        return 2;
    }
    if (all) {
        if (!tick_set) cfg.tick = 100;
        // Every book is allocated at full capacity, and a real day has several
        // thousand symbols. These keep a book to about 1 MB; busy names may need
        // more and will show up in the "full" and "out of range" counts.
        if (!levels_set) cfg.max_levels = 1u << 14;
        if (!orders_set) cfg.max_orders = 1u << 13;
        symbol.clear();
    }

    if (std::string_view(path) == "-") baseline = false;  // can't read stdin twice

    // built first so a bad config fails before the parse-only pass
    itch::BookBuilder bb(cfg, symbol);

    Run base;
    if (parse_only || baseline) {
        Sink sink;
        base = replay(path, sink);
        print_run(parse_only ? "parse" : "parse only", base);
        if (parse_only) {
            print_types(base);
            std::printf("checksum %llx\n", static_cast<unsigned long long>(sink.sum));
            return base.ok ? 0 : 1;
        }
    }

    Run run = replay(path, bb);
    // allocating and zeroing the books isn't part of processing the feed
    run.seconds -= bb.setup_seconds();
    std::printf("book setup   %.3f s (excluded below)\n", bb.setup_seconds());
    print_run(all ? "all books" : "book", run);
    print_types(run);
    print_stats(bb.stats());

    const auto& s = bb.stats();
    if (s.book_ops > 0) {
        std::printf("book updates: %.1f M/s of total time",
                    static_cast<double>(s.book_ops) / run.seconds / 1e6);
        if (baseline && run.seconds > base.seconds)
            std::printf(", ~%.1f ns each over the parse-only pass",
                        (run.seconds - base.seconds) * 1e9 / static_cast<double>(s.book_ops));
        std::printf("\n");
    }

    bool valid = true;
    std::size_t books = 0, resting = 0;
    bb.for_each_book([&](std::uint16_t, const itch::BookBuilder::Book& b) {
        valid = valid && b.validate();
        ++books;
        resting += b.order_count();
    });
    const bool found = all || bb.target() != 0;
    if (all) std::printf("%zu books, %zu resting orders\n", books, resting);
    else if (!found) std::fprintf(stderr, "symbol %s not found\n", symbol.c_str());
    else print_book(bb, bb.target(), depth);
    std::printf("validate: %s\n", valid ? "ok" : "FAILED");
    return valid && run.ok && found ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 2;
}
