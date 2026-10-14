#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "lob/order_id_map.hpp"
#include "lob/price_bitmap.hpp"
#include "lob/types.hpp"

namespace lob {

// Price-time priority book over a fixed tick range. All memory is allocated
// in the constructor. Listener::on_trade() is called for every fill.
template <class Listener = NullListener>
class OrderBook {
public:
    // Throws std::invalid_argument on a bad range or max_orders, before
    // allocating anything.
    OrderBook(Price min_price, Price max_price, std::size_t max_orders, Listener listener = {})
        : min_price_(checked_min_price(min_price, max_price, max_orders)),
          max_price_(max_price),
          max_orders_(max_orders),
          listener_(std::move(listener)),
          orders_(max_orders),
          ids_(max_orders),
          bids_(levels(min_price, max_price)),
          asks_(levels(min_price, max_price)),
          bid_bits_(bids_.size()),
          ask_bits_(asks_.size()) {
        for (std::uint32_t i = 0; i < max_orders; ++i) orders_[i].next = i + 1;
        orders_[max_orders - 1].next = nil;
    }

    // 16 B per level per side, so 2^28 levels is already 8 GiB.
    static constexpr std::uint64_t kMaxLevels = std::uint64_t{1} << 28;

    Status add(OrderId id, Side side, Price price, Qty qty, Tif tif = Tif::Gtc) {
        if (qty == 0) [[unlikely]]
            return Status::BadQty;
        if (!in_range(price)) [[unlikely]]
            return Status::BadPrice;
        if (ids_.find(id) != OrderIdMap::npos) [[unlikely]]
            return Status::DuplicateId;

        // IOC and FOK never rest, so they skip the BookFull check
        if (tif == Tif::Ioc || tif == Tif::Fok) {
            if (tif == Tif::Fok && !can_fill(side, price, qty)) return Status::Killed;
            match(id, side, price, qty);
            return Status::Ok;
        }
        if (tif == Tif::PostOnly && crosses(side, price)) return Status::WouldCross;
        const std::size_t li = index(price);
        Level& lvl = level(side, li);
        if (would_overflow(lvl, qty)) return Status::BadQty;

        if (count_ == max_orders_ && !can_fill(side, price, qty)) return Status::BookFull;
        qty = match(id, side, price, qty);
        if (qty > 0) rest(id, side, li, lvl, qty);
        return Status::Ok;
    }

    // Unfilled qty is dropped. The id only tags trades and needn't be unique.
    Status market(OrderId id, Side side, Qty qty) {
        if (qty == 0) [[unlikely]]
            return Status::BadQty;
        sweep(id, side, side == Side::Buy ? max_price_ : min_price_, qty);
        return Status::Ok;
    }

    Status cancel(OrderId id) {
        const std::uint32_t i = ids_.find(id);
        if (i == OrderIdMap::npos) [[unlikely]]
            return Status::UnknownId;
        remove(i);
        return Status::Ok;
    }

    // Shrinking qty at the same price keeps queue position; anything else
    // goes to the back.
    Status modify(OrderId id, Price price, Qty qty) {
        if (qty == 0) [[unlikely]]
            return Status::BadQty;
        if (!in_range(price)) [[unlikely]]
            return Status::BadPrice;
        const std::uint32_t i = ids_.find(id);
        if (i == OrderIdMap::npos) [[unlikely]]
            return Status::UnknownId;

        Order& o = orders_[i];
        if (index(price) == o.level && qty <= o.qty) {
            level(o.side, index(price)).qty -= o.qty - qty;
            o.qty = qty;
            return Status::Ok;
        }

        const std::size_t li = index(price);
        Level& lvl = level(o.side, li);
        if (would_overflow(lvl, qty, li == o.level ? o.qty : 0)) return Status::BadQty;

        const Side side = o.side;
        remove(i);
        qty = match(id, side, price, qty);
        if (qty > 0) rest(id, side, li, lvl, qty);
        return Status::Ok;
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        if (best_bid_ == PriceBitmap::npos) return std::nullopt;
        return price_at(best_bid_);
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        if (best_ask_ == PriceBitmap::npos) return std::nullopt;
        return price_at(best_ask_);
    }

    [[nodiscard]] Qty depth_at(Side side, Price price) const noexcept {
        if (!in_range(price)) return 0;
        return level(side, index(price)).qty;
    }

    [[nodiscard]] std::optional<Qty> order_qty(OrderId id) const noexcept {
        const std::uint32_t i = ids_.find(id);
        if (i == OrderIdMap::npos) return std::nullopt;
        return orders_[i].qty;
    }

    [[nodiscard]] std::optional<OrderInfo> order(OrderId id) const noexcept {
        const std::uint32_t i = ids_.find(id);
        if (i == OrderIdMap::npos) return std::nullopt;
        const Order& o = orders_[i];
        return OrderInfo{o.side, price_at(o.level), o.qty};
    }

    // For estimating memory use without building a book.
    [[nodiscard]] static constexpr std::size_t order_bytes() noexcept { return sizeof(Order); }
    [[nodiscard]] static constexpr std::size_t level_bytes() noexcept { return sizeof(Level); }

    [[nodiscard]] std::size_t order_count() const noexcept { return count_; }
    [[nodiscard]] Price min_price() const noexcept { return min_price_; }
    [[nodiscard]] Price max_price() const noexcept { return max_price_; }

    [[nodiscard]] Listener& listener() { return listener_; }
    [[nodiscard]] const Listener& listener() const { return listener_; }

    // Calls f(price, total_qty) for each non-empty level, best first.
    template <class F>
    void for_each_level(Side side, F&& f, std::size_t max_levels = SIZE_MAX) const {
        if (side == Side::Buy) {
            for (std::size_t i = best_bid_; i != PriceBitmap::npos && max_levels--;) {
                f(price_at(i), bids_[i].qty);
                i = i == 0 ? PriceBitmap::npos : bid_bits_.prev(i - 1);
            }
        } else {
            for (std::size_t i = best_ask_; i != PriceBitmap::npos && max_levels--;) {
                f(price_at(i), asks_[i].qty);
                i = ask_bits_.next(i + 1);
            }
        }
    }

    // Consistency check across all structures. Slow, for tests.
    [[nodiscard]] bool validate() const noexcept {
        std::size_t seen = 0;
        auto check_side = [&](Side side, const std::vector<Level>& lvls, const PriceBitmap& bits) {
            for (std::size_t li = 0; li < lvls.size(); ++li) {
                const Level& lvl = lvls[li];
                if (bits.test(li) != (lvl.head != nil)) return false;
                Qty total = 0;
                std::uint32_t prev = nil;
                for (std::uint32_t i = lvl.head; i != nil; i = orders_[i].next) {
                    const Order& o = orders_[i];
                    if (o.prev != prev || o.side != side || o.qty == 0) return false;
                    if (o.level != li || ids_.find(o.id) != i) return false;
                    total += o.qty;
                    prev = i;
                    ++seen;
                }
                if (lvl.tail != prev || lvl.qty != total) return false;
            }
            return true;
        };
        if (!check_side(Side::Buy, bids_, bid_bits_)) return false;
        if (!check_side(Side::Sell, asks_, ask_bits_)) return false;
        if (seen != count_ || ids_.size() != count_) return false;
        if (best_bid_ != bid_bits_.prev(SIZE_MAX)) return false;
        if (best_ask_ != ask_bits_.next(0)) return false;
        return best_bid_ == PriceBitmap::npos || best_ask_ == PriceBitmap::npos ||
               best_bid_ < best_ask_;
    }

private:
    static constexpr std::uint32_t nil = UINT32_MAX;

    // 32 bytes, two per cache line. Price is stored as the level index.
    struct Order {
        OrderId id = 0;
        Qty qty = 0;
        std::uint32_t prev = nil;
        std::uint32_t next = nil;  // doubles as the free list link
        std::uint32_t level = 0;
        Side side = Side::Buy;
    };
    static_assert(sizeof(Order) == 32);

    struct Level {
        std::uint32_t head = nil;
        std::uint32_t tail = nil;
        Qty qty = 0;
    };

    // in uint64 since hi - lo can overflow int64
    static std::uint64_t span(Price lo, Price hi) {
        return static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
    }

    static Price checked_min_price(Price lo, Price hi, std::size_t max_orders) {
        if (hi < lo) throw std::invalid_argument("max_price < min_price");
        if (span(lo, hi) >= kMaxLevels) throw std::invalid_argument("price range too wide");
        if (max_orders == 0 || max_orders >= nil) throw std::invalid_argument("bad max_orders");
        return lo;
    }

    static std::size_t levels(Price lo, Price hi) {
        return static_cast<std::size_t>(span(lo, hi)) + 1;
    }

    bool in_range(Price p) const { return p >= min_price_ && p <= max_price_; }
    std::size_t index(Price p) const { return static_cast<std::size_t>(span(min_price_, p)); }
    Price price_at(std::size_t i) const { return min_price_ + static_cast<Price>(i); }

    Level& level(Side side, std::size_t i) { return side == Side::Buy ? bids_[i] : asks_[i]; }
    const Level& level(Side side, std::size_t i) const {
        return side == Side::Buy ? bids_[i] : asks_[i];
    }

    // Own side only: if it has qty at this price the order can't cross.
    // `already` is the qty of an order being replaced in the same level.
    static bool would_overflow(const Level& lvl, Qty qty, Qty already = 0) {
        return lvl.qty - already > std::numeric_limits<Qty>::max() - qty;
    }

    bool crosses(Side side, Price limit) const {
        const std::size_t li = index(limit);
        if (side == Side::Buy) return best_ask_ != PriceBitmap::npos && best_ask_ <= li;
        return best_bid_ != PriceBitmap::npos && best_bid_ >= li;
    }

    // Whether the opposite side holds at least qty up to `limit`.
    bool can_fill(Side side, Price limit, Qty qty) const {
        const std::size_t li = index(limit);
        Qty seen = 0;
        if (side == Side::Buy) {
            for (std::size_t i = best_ask_; i != PriceBitmap::npos && i <= li;
                 i = ask_bits_.next(i + 1)) {
                if (asks_[i].qty >= qty - seen) return true;
                seen += asks_[i].qty;
            }
        } else {
            for (std::size_t i = best_bid_; i != PriceBitmap::npos && i >= li;) {
                if (bids_[i].qty >= qty - seen) return true;
                seen += bids_[i].qty;
                i = i == 0 ? PriceBitmap::npos : bid_bits_.prev(i - 1);
            }
        }
        return false;
    }

    // Fills against the opposite side up to `limit`, returns what's left.
    // The book is consistent before each on_trade, so if the listener throws
    // the rest of the taker is dropped and the book stays valid.
    Qty match(OrderId taker, Side side, Price limit, Qty qty) {
        // most limit orders don't cross
        if (!crosses(side, limit)) [[likely]]
            return qty;
        return sweep(taker, side, limit, qty);
    }

    Qty sweep(OrderId taker, Side side, Price limit, Qty qty) {
        const std::size_t li = index(limit);
        if (side == Side::Buy) {
            while (qty > 0 && best_ask_ != PriceBitmap::npos && best_ask_ <= li) {
                qty = take(taker, Side::Sell, qty);
            }
        } else {
            while (qty > 0 && best_bid_ != PriceBitmap::npos && best_bid_ >= li) {
                qty = take(taker, Side::Buy, qty);
            }
        }
        return qty;
    }

    // Trades against the front of the best level on `maker_side`.
    Qty take(OrderId taker, Side maker_side, Qty qty) {
        const std::size_t li = maker_side == Side::Buy ? best_bid_ : best_ask_;
        Level& lvl = level(maker_side, li);
        const std::uint32_t i = lvl.head;
        Order& o = orders_[i];
        const Qty q = std::min(qty, o.qty);
        const Trade trade{taker, o.id, price_at(li), q};
        lvl.qty -= q;
        if (q == o.qty) {
            lvl.head = o.next;
            if (lvl.head == nil) {
                lvl.tail = nil;
                level_emptied(maker_side, li);
            } else {
                orders_[lvl.head].prev = nil;
            }
            ids_.erase(o.id);
            release(i);
        } else {
            o.qty -= q;
        }
        listener_.on_trade(trade);
        return qty - q;
    }

    void level_emptied(Side side, std::size_t li) {
        if (side == Side::Buy) {
            bid_bits_.clear(li);
            if (li == best_bid_) best_bid_ = li == 0 ? PriceBitmap::npos : bid_bits_.prev(li - 1);
        } else {
            ask_bits_.clear(li);
            if (li == best_ask_) best_ask_ = ask_bits_.next(li + 1);
        }
    }

    void rest(OrderId id, Side side, std::size_t li, Level& lvl, Qty qty) {
        const std::uint32_t i = free_;
        free_ = orders_[i].next;
        ++count_;

        orders_[i] = Order{id, qty, lvl.tail, nil, static_cast<std::uint32_t>(li), side};
        ids_.insert(id, i);

        if (lvl.tail == nil) {
            lvl.head = i;
            if (side == Side::Buy) {
                bid_bits_.set(li);
                if (best_bid_ == PriceBitmap::npos || li > best_bid_) best_bid_ = li;
            } else {
                ask_bits_.set(li);
                if (best_ask_ == PriceBitmap::npos || li < best_ask_) best_ask_ = li;
            }
        } else {
            orders_[lvl.tail].next = i;
        }
        lvl.tail = i;
        lvl.qty += qty;
    }

    void remove(std::uint32_t i) {
        Order& o = orders_[i];
        const std::size_t li = o.level;
        Level& lvl = level(o.side, li);

        if (o.prev != nil) orders_[o.prev].next = o.next;
        else lvl.head = o.next;
        if (o.next != nil) orders_[o.next].prev = o.prev;
        else lvl.tail = o.prev;
        lvl.qty -= o.qty;

        if (lvl.head == nil) level_emptied(o.side, li);

        ids_.erase(o.id);
        release(i);
    }

    void release(std::uint32_t i) {
        orders_[i].next = free_;
        free_ = i;
        --count_;
    }

    Price min_price_;
    Price max_price_;
    std::size_t max_orders_;
    Listener listener_;

    std::vector<Order> orders_;
    std::uint32_t free_ = 0;
    std::size_t count_ = 0;
    OrderIdMap ids_;

    std::vector<Level> bids_;
    std::vector<Level> asks_;
    PriceBitmap bid_bits_;
    PriceBitmap ask_bits_;
    std::size_t best_bid_ = PriceBitmap::npos;
    std::size_t best_ask_ = PriceBitmap::npos;
};

}  // namespace lob
