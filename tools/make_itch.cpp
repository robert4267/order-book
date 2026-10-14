// Writes a synthetic ITCH 5.0 file for benchmarking lob_itch_replay when no
// real NASDAQ capture is at hand.
//
//   lob_make_itch OUT [MESSAGES] [SEED]
//
// A few symbols with a random-walk mid and a rough real-session message mix.
// Messages are fed through a BookBuilder as they're written, so they only
// refer to resting orders and the printed final book is what a replay should
// produce.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lob/itch.hpp"
#include "lob/itch_book.hpp"

namespace {

using namespace lob;
namespace enc = itch::encode;

struct Instrument {
    Instrument(std::string_view n, std::uint32_t p, std::uint32_t s, double w)
        : name(n), price(p), step(s), weight(w) {}

    std::string_view name;
    std::uint32_t price;  // starting mid, ITCH units
    std::uint32_t step;   // price increment, ITCH units
    double weight;        // share of messages
    std::uint16_t locate = 0;
    std::int64_t mid = 0;  // in steps
    std::vector<std::uint64_t> live;
};

class Generator {
public:
    explicit Generator(std::uint64_t seed) : rng_(seed), shadow_(shadow_config()) {
        instruments_ = {
            {"AAPL", 1'400'000, 100, 30}, {"SPY", 4'000'000, 100, 25}, {"MSFT", 2'500'000, 100, 20},
            {"TSLA", 1'800'000, 100, 15}, {"AMD", 1'000'000, 100, 8},  {"PENY", 5'432, 1, 2},
        };
        std::vector<double> w;
        for (std::size_t i = 0; i < instruments_.size(); ++i) {
            auto& in = instruments_[i];
            in.locate = static_cast<std::uint16_t>(i + 1);
            in.mid = in.price / in.step;
            w.push_back(in.weight);
        }
        pick_ = std::discrete_distribution<std::size_t>(w.begin(), w.end());
    }

    void header() {
        enc::system_event(out_, 'O', ts());
        for (const auto& in : instruments_)
            emit([&] { enc::stock_directory(out_, in.locate, in.name, ts()); });
        enc::system_event(out_, 'S', ts());
        enc::system_event(out_, 'Q', ts());
    }

    void step() {
        Instrument& in = instruments_[pick_(rng_)];
        if (uniform() < 0.01) walk(in);

        const double r = uniform();
        const double p_add = in.live.size() < kTargetResting ? 0.6 : 0.4;
        if (in.live.empty() || r < p_add) return add(in);
        const double q = (r - p_add) / (1 - p_add);
        if (q < 0.74) delete_order(in);
        else if (q < 0.89) replace(in);
        else if (q < 0.96) execute(in);
        else if (q < 0.99) partial_cancel(in);
        else hidden_trade(in);
    }

    void trailer() {
        enc::system_event(out_, 'M', ts());
        enc::system_event(out_, 'E', ts());
    }

    std::string& buffer() { return out_; }
    const itch::BookBuilder& shadow() const { return shadow_; }
    const std::vector<Instrument>& instruments() const { return instruments_; }

private:
    static constexpr std::size_t kTargetResting = 3000;

    static itch::BookConfig shadow_config() {
        itch::BookConfig c;
        c.window = 0.2;
        c.max_levels = 1u << 21;
        c.max_orders = 1u << 16;
        return c;
    }

    double uniform() { return std::uniform_real_distribution<double>(0, 1)(rng_); }
    std::uint64_t ts() { return now_ += 1 + rng_() % 2000; }

    // Writes one message and applies it to the shadow books.
    template <class F>
    void emit(F&& write) {
        const std::size_t start = out_.size();
        write();
        const auto r = itch::parse(out_.data() + start, out_.size() - start, shadow_);
        if (r.error != itch::Error::None) throw std::logic_error("generator wrote a bad frame");
    }

    void walk(Instrument& in) {
        const std::int64_t base = in.price / in.step;
        const std::int64_t band = std::max<std::int64_t>(base / 20, 5);  // stay within ~5%
        in.mid += uniform() < 0.5 ? -1 : 1;
        in.mid = std::clamp(in.mid, base - band, base + band);
    }

    std::uint32_t shares() {
        const auto lots = std::geometric_distribution<std::uint32_t>(0.4)(rng_) + 1;
        return uniform() < 0.1 ? lots * 7 + static_cast<std::uint32_t>(rng_() % 100) : lots * 100;
    }

    // A price on `side` that doesn't cross the current book.
    std::uint32_t passive_price(const Instrument& in, Side side) {
        const auto* b = shadow_.book(in.locate);
        const auto k = static_cast<std::int64_t>(std::geometric_distribution<int>(0.25)(rng_));
        std::int64_t px;
        if (side == Side::Buy) {
            px = in.mid - k;
            if (b && b->best_ask()) px = std::min<std::int64_t>(px, *b->best_ask() / in.step - 1);
        } else {
            px = in.mid + 1 + k;
            if (b && b->best_bid()) px = std::max<std::int64_t>(px, *b->best_bid() / in.step + 1);
        }
        return static_cast<std::uint32_t>(std::max<std::int64_t>(px, 1) * in.step);
    }

    std::size_t random_live(const Instrument& in) { return rng_() % in.live.size(); }

    void forget(Instrument& in, std::size_t i) {
        in.live[i] = in.live.back();
        in.live.pop_back();
    }

    OrderInfo order(const Instrument& in, std::uint64_t ref) const {
        return *shadow_.book(in.locate)->order(ref);
    }

    void add(Instrument& in) {
        const Side side = uniform() < 0.5 ? Side::Buy : Side::Sell;
        const std::uint64_t ref = ++next_ref_;
        const std::uint32_t px = passive_price(in, side);
        const std::string_view mpid = uniform() < 0.02 ? "GSCO" : "";
        emit([&] {
            enc::add_order(out_, in.locate, ref, side == Side::Buy ? 'B' : 'S', shares(), in.name,
                           px, mpid, ts());
        });
        if (shadow_.book(in.locate)->order(ref)) in.live.push_back(ref);
    }

    void delete_order(Instrument& in) {
        const std::size_t i = random_live(in);
        emit([&] { enc::del(out_, in.locate, in.live[i], ts()); });
        forget(in, i);
    }

    void replace(Instrument& in) {
        const std::size_t i = random_live(in);
        const OrderInfo o = order(in, in.live[i]);
        const std::uint64_t ref = ++next_ref_;
        const std::uint32_t px = passive_price(in, o.side);
        emit([&] { enc::replace(out_, in.locate, in.live[i], ref, shares(), px, ts()); });
        if (shadow_.book(in.locate)->order(ref)) in.live[i] = ref;
        else forget(in, i);
    }

    // Executions land on the best-priced of a few random orders, a cheap
    // stand-in for hitting the front of the book.
    void execute(Instrument& in) {
        std::size_t best = random_live(in);
        for (int n = 0; n < 4; ++n) {
            const std::size_t c = random_live(in);
            const OrderInfo a = order(in, in.live[c]), b = order(in, in.live[best]);
            if (a.side == b.side && (a.side == Side::Buy ? a.price > b.price : a.price < b.price))
                best = c;
        }
        const OrderInfo o = order(in, in.live[best]);
        const auto qty = static_cast<std::uint32_t>(uniform() < 0.6 ? o.qty : 1 + rng_() % o.qty);
        const std::uint64_t ref = in.live[best];
        if (uniform() < 0.05) {
            emit([&] {
                enc::executed_with_price(out_, in.locate, ref, qty, ++match_, true,
                                         static_cast<std::uint32_t>(o.price), ts());
            });
        } else {
            emit([&] { enc::executed(out_, in.locate, ref, qty, ++match_, ts()); });
        }
        if (qty == o.qty) forget(in, best);
    }

    void partial_cancel(Instrument& in) {
        const std::size_t i = random_live(in);
        const OrderInfo o = order(in, in.live[i]);
        if (o.qty < 2) return delete_order(in);
        const auto qty = static_cast<std::uint32_t>(1 + rng_() % (o.qty - 1));
        emit([&] { enc::cancel(out_, in.locate, in.live[i], qty, ts()); });
    }

    void hidden_trade(Instrument& in) {
        const auto px = static_cast<std::uint32_t>(in.mid * in.step);
        emit([&] {
            enc::trade(out_, in.locate, uniform() < 0.5 ? 'B' : 'S', shares(), in.name, px,
                       ++match_, ts());
        });
    }

    std::mt19937_64 rng_;
    itch::BookBuilder shadow_;
    std::vector<Instrument> instruments_;
    std::discrete_distribution<std::size_t> pick_;
    std::string out_;
    std::uint64_t now_ = 4ull * 3600 * 1'000'000'000;
    std::uint64_t next_ref_ = 0;
    std::uint64_t match_ = 0;
};

// stoull would happily read "-1" as 2^64 - 1
std::uint64_t parse_count(const char* s) {
    std::uint64_t v = 0;
    const char* end = s + std::strlen(s);
    const auto [ptr, ec] = std::from_chars(s, end, v);
    if (ec != std::errc{} || ptr != end)
        throw std::invalid_argument(std::string("bad number: ") + s);
    return v;
}

}  // namespace

int main(int argc, char** argv) try {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s OUT [MESSAGES] [SEED]\n", argv[0]);
        return 2;
    }
    const std::uint64_t n = argc > 2 ? parse_count(argv[2]) : 10'000'000;
    const std::uint64_t seed = argc > 3 ? parse_count(argv[3]) : 1;

    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f) throw std::runtime_error(std::string("can't open ") + argv[1]);
    auto flush = [&](std::string& buf) {
        if (std::fwrite(buf.data(), 1, buf.size(), f) != buf.size())
            throw std::runtime_error("write failed");
        buf.clear();
    };

    Generator gen(seed);
    gen.header();
    for (std::uint64_t i = 0; i < n; ++i) {
        gen.step();
        if (gen.buffer().size() > (16u << 20)) flush(gen.buffer());
    }
    gen.trailer();
    flush(gen.buffer());
    if (std::fclose(f) != 0) throw std::runtime_error("close failed");

    // same $X.XXXX format as lob_itch_replay
    auto dollars = [&](std::optional<Price> p) {
        if (!p) return std::string("-");
        const std::uint64_t v = gen.shadow().to_itch(*p);
        char out[32];
        std::snprintf(out, sizeof out, "$%llu.%04llu", static_cast<unsigned long long>(v / 10000),
                      static_cast<unsigned long long>(v % 10000));
        return std::string(out);
    };
    for (const auto& in : gen.instruments()) {
        const auto* b = gen.shadow().book(in.locate);
        if (!b) continue;
        std::printf("%-5.*s resting %6zu  bid %s  ask %s\n", static_cast<int>(in.name.size()),
                    in.name.data(), b->order_count(), dollars(b->best_bid()).c_str(),
                    dollars(b->best_ask()).c_str());
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
}
