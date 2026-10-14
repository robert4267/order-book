#pragma once

// Builds OrderBooks from a decoded ITCH 5.0 stream.
//
// Adds go in as post-only, since the feed never asks the book to match; a
// crossing add (halts, auction crosses) is refused and counted. Executions and
// partial cancels shrink the order in place, keeping queue position.
//
// Each book covers a price window around the first add (or `centre`), in units
// of `tick`. tick = 100 is one cent, lossless for prices >= $1. Orders outside
// the window or off the grid are skipped, as is anything referring to them.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lob/itch.hpp"
#include "lob/order_book.hpp"

namespace lob::itch {

struct BookConfig {
    std::uint32_t tick = 1;             // ITCH price units per book tick
    double window = 0.5;                // +/- this fraction of the centre price
    std::uint32_t centre = 0;           // ITCH units; 0 means the first add's price
    std::size_t max_levels = 1u << 22;  // caps the window for expensive stocks
    // per book; oversizing costs cache misses, undersizing shows up as "full"
    std::size_t max_orders = 1u << 18;
};

struct BookStats {
    std::uint64_t adds = 0;
    std::uint64_t executes = 0;
    std::uint64_t cancels = 0;
    std::uint64_t deletes = 0;
    std::uint64_t replaces = 0;
    std::uint64_t book_ops = 0;  // calls into OrderBook that changed it

    std::uint64_t out_of_range = 0;
    std::uint64_t off_tick = 0;
    std::uint64_t crossed = 0;
    std::uint64_t duplicate = 0;
    std::uint64_t full = 0;
    std::uint64_t bad_qty = 0;
    std::uint64_t unknown_ref = 0;  // usually an order we skipped earlier
    std::uint64_t over_reduce = 0;  // executed/cancelled more than was resting

    std::uint64_t skipped() const {
        return out_of_range + off_tick + crossed + duplicate + full + bad_qty;
    }
};

class BookBuilder {
public:
    using Book = OrderBook<>;

    // An empty symbol builds a book for every instrument in the feed.
    explicit BookBuilder(BookConfig cfg = {}, std::string_view symbol = {})
        : cfg_(cfg), symbol_(symbol), books_(kLocates), names_(kLocates) {
        if (cfg_.tick == 0) throw std::invalid_argument("tick must be at least 1");
        if (cfg_.max_levels < 2) throw std::invalid_argument("max_levels must be at least 2");
        if (cfg_.max_orders == 0) throw std::invalid_argument("max_orders must be at least 1");
        if (!(cfg_.window > 0)) throw std::invalid_argument("window must be positive");
    }

    void on(const StockDirectory& m) {
        names_[m.h.locate] = m.stock;
        if (!symbol_.empty() && trim(m.stock) == symbol_) target_ = m.h.locate;
    }

    void on(const AddOrder& m) {
        // The directory normally comes first, but don't rely on it
        if (!symbol_.empty() && target_ == 0 && trim(m.stock) == symbol_) {
            target_ = m.h.locate;
            names_[m.h.locate] = m.stock;
        }
        if (!wanted(m.h.locate)) return;
        ++stats_.adds;
        if (names_[m.h.locate][0] == '\0') names_[m.h.locate] = m.stock;
        Book* b = book_for(m.h.locate, m.price);
        if (b == nullptr) return;
        add(*b, m.ref, m.side == 'B' ? Side::Buy : Side::Sell, m.shares, m.price);
    }

    void on(const OrderExecuted& m) {
        if (!wanted(m.h.locate)) return;
        ++stats_.executes;
        reduce(m.h.locate, m.ref, m.shares);
    }

    void on(const OrderCancel& m) {
        if (!wanted(m.h.locate)) return;
        ++stats_.cancels;
        reduce(m.h.locate, m.ref, m.shares);
    }

    void on(const OrderDelete& m) {
        if (!wanted(m.h.locate)) return;
        ++stats_.deletes;
        Book* b = books_[m.h.locate].get();
        if (b == nullptr || b->cancel(m.ref) != Status::Ok) {
            ++stats_.unknown_ref;
            return;
        }
        ++stats_.book_ops;
    }

    void on(const OrderReplace& m) {
        if (!wanted(m.h.locate)) return;
        ++stats_.replaces;
        Book* b = books_[m.h.locate].get();
        const auto old = b ? b->order(m.old_ref) : std::nullopt;
        if (!old) {
            ++stats_.unknown_ref;
            return;
        }
        if (b->cancel(m.old_ref) == Status::Ok) ++stats_.book_ops;
        add(*b, m.new_ref, old->side, m.shares, m.price);
    }

    const BookStats& stats() const { return stats_; }

    // Time spent allocating books, which happens mid-stream on first use.
    double setup_seconds() const { return std::chrono::duration<double>(setup_).count(); }

    // Locate of the requested symbol once it has been seen, otherwise 0.
    std::uint16_t target() const { return target_; }

    const Book* book(std::uint16_t locate) const { return books_[locate].get(); }
    Book* book(std::uint16_t locate) { return books_[locate].get(); }
    std::string_view name(std::uint16_t locate) const {
        return names_[locate][0] == '\0' ? std::string_view{} : trim(names_[locate]);
    }

    // Book prices are in ticks; this turns one back into ITCH units.
    std::uint64_t to_itch(Price p) const { return static_cast<std::uint64_t>(p) * cfg_.tick; }

    template <class F>
    void for_each_book(F&& f) const {
        for (std::size_t i = 0; i < books_.size(); ++i)
            if (books_[i]) f(static_cast<std::uint16_t>(i), *books_[i]);
    }

private:
    static constexpr std::size_t kLocates = 1u << 16;

    bool wanted(std::uint16_t locate) const {
        return symbol_.empty() || (locate == target_ && target_ != 0);
    }

    Book* book_for(std::uint16_t locate, std::uint32_t first_price) {
        if (books_[locate]) return books_[locate].get();
        const auto centre =
            static_cast<Price>((cfg_.centre ? cfg_.centre : first_price) / cfg_.tick);
        if (centre == 0) {
            ++stats_.out_of_range;
            return nullptr;
        }
        auto half = static_cast<Price>(static_cast<double>(centre) * cfg_.window);
        half = std::clamp<Price>(half, 1, static_cast<Price>(cfg_.max_levels / 2));
        const Price lo = std::max<Price>(1, centre - half);
        const auto t0 = std::chrono::steady_clock::now();
        books_[locate] = std::make_unique<Book>(lo, centre + half, cfg_.max_orders);
        setup_ += std::chrono::steady_clock::now() - t0;
        return books_[locate].get();
    }

    void add(Book& b, std::uint64_t ref, Side side, std::uint32_t shares, std::uint32_t price) {
        if (price % cfg_.tick != 0) {
            ++stats_.off_tick;
            return;
        }
        const auto px = static_cast<Price>(price / cfg_.tick);
        switch (b.add(ref, side, px, shares, Tif::PostOnly)) {
            case Status::Ok: ++stats_.book_ops; break;
            case Status::BadPrice: ++stats_.out_of_range; break;
            case Status::WouldCross: ++stats_.crossed; break;
            case Status::DuplicateId: ++stats_.duplicate; break;
            case Status::BookFull: ++stats_.full; break;
            case Status::BadQty: ++stats_.bad_qty; break;
            default: break;
        }
    }

    void reduce(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
        Book* b = books_[locate].get();
        const auto o = b ? b->order(ref) : std::nullopt;
        if (!o) {
            ++stats_.unknown_ref;
            return;
        }
        if (shares > o->qty) ++stats_.over_reduce;
        const Status s =
            shares >= o->qty ? b->cancel(ref) : b->modify(ref, o->price, o->qty - shares);
        if (s == Status::Ok) ++stats_.book_ops;
    }

    BookConfig cfg_;
    std::string symbol_;
    std::uint16_t target_ = 0;
    std::vector<std::unique_ptr<Book>> books_;
    std::vector<Symbol> names_;
    BookStats stats_;
    std::chrono::steady_clock::duration setup_{};
};

}  // namespace lob::itch
