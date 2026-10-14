#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "lob/types.hpp"

namespace lob {

// std::map of price -> std::list of orders, plus an unordered_map for cancels.
// Same interface and rules as OrderBook. Reference model for the tests and
// baseline for the benchmark.
template <class Listener = NullListener>
class MapOrderBook {
public:
    MapOrderBook(Price min_price, Price max_price, std::size_t max_orders, Listener listener = {})
        : min_price_(min_price),
          max_price_(max_price),
          max_orders_(max_orders),
          listener_(std::move(listener)) {
        if (max_price < min_price) throw std::invalid_argument("max_price < min_price");
        if (max_orders == 0) throw std::invalid_argument("bad max_orders");
    }

    Status add(OrderId id, Side side, Price price, Qty qty, Tif tif = Tif::Gtc) {
        if (qty == 0) return Status::BadQty;
        if (!in_range(price)) return Status::BadPrice;
        if (ids_.count(id)) return Status::DuplicateId;

        // FOK: sum what's available before touching anything
        if (tif == Tif::Ioc) {
            match(id, side, price, qty);
            return Status::Ok;
        }
        if (tif == Tif::Fok) {
            if (available(side, price) < qty) return Status::Killed;
            match(id, side, price, qty);
            return Status::Ok;
        }
        if (tif == Tif::PostOnly && available(side, price) > 0) return Status::WouldCross;
        if (depth_at(side, price) > std::numeric_limits<Qty>::max() - qty) return Status::BadQty;

        // a full book only matters if some of the order would have to rest
        if (ids_.size() == max_orders_ && available(side, price) < qty) return Status::BookFull;

        qty = match(id, side, price, qty);
        if (qty > 0) rest(id, side, price, qty);
        return Status::Ok;
    }

    Status market(OrderId id, Side side, Qty qty) {
        if (qty == 0) return Status::BadQty;
        match(id, side, side == Side::Buy ? max_price_ : min_price_, qty);
        return Status::Ok;
    }

    Status cancel(OrderId id) {
        auto it = ids_.find(id);
        if (it == ids_.end()) return Status::UnknownId;
        erase(it);
        return Status::Ok;
    }

    Status modify(OrderId id, Price price, Qty qty) {
        if (qty == 0) return Status::BadQty;
        if (!in_range(price)) return Status::BadPrice;
        auto it = ids_.find(id);
        if (it == ids_.end()) return Status::UnknownId;

        Order& o = *it->second;
        if (price == o.price && qty <= o.qty) {
            level(o.side, o.price).qty -= o.qty - qty;
            o.qty = qty;
            return Status::Ok;
        }

        const Qty others = depth_at(o.side, price) - (price == o.price ? o.qty : 0);
        if (others > std::numeric_limits<Qty>::max() - qty) return Status::BadQty;

        const Side side = o.side;
        erase(it);
        qty = match(id, side, price, qty);
        if (qty > 0) rest(id, side, price, qty);
        return Status::Ok;
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    [[nodiscard]] Qty depth_at(Side side, Price price) const noexcept {
        auto qty_at = [](const auto& book, Price p) {
            auto it = book.find(p);
            return it == book.end() ? Qty{0} : it->second.qty;
        };
        return side == Side::Buy ? qty_at(bids_, price) : qty_at(asks_, price);
    }

    [[nodiscard]] std::optional<Qty> order_qty(OrderId id) const noexcept {
        auto it = ids_.find(id);
        if (it == ids_.end()) return std::nullopt;
        return it->second->qty;
    }

    [[nodiscard]] std::optional<OrderInfo> order(OrderId id) const noexcept {
        auto it = ids_.find(id);
        if (it == ids_.end()) return std::nullopt;
        const Order& o = *it->second;
        return OrderInfo{o.side, o.price, o.qty};
    }

    [[nodiscard]] std::size_t order_count() const noexcept { return ids_.size(); }

    [[nodiscard]] Listener& listener() { return listener_; }
    [[nodiscard]] const Listener& listener() const { return listener_; }

private:
    struct Order {
        OrderId id;
        Side side;
        Price price;
        Qty qty;
    };
    using Queue = std::list<Order>;

    // running total so depth_at() and the overflow check don't walk the queue
    struct Level {
        Queue orders;
        Qty qty = 0;
    };

    Level& level(Side side, Price price) { return side == Side::Buy ? bids_[price] : asks_[price]; }

    bool in_range(Price p) const { return p >= min_price_ && p <= max_price_; }

    Qty match(OrderId taker, Side side, Price limit, Qty qty) {
        if (side == Side::Buy) {
            return match_against(asks_, taker, qty, [&](Price p) { return p <= limit; });
        }
        return match_against(bids_, taker, qty, [&](Price p) { return p >= limit; });
    }

    // Total resting qty on the opposite side at prices `side` would trade at,
    // saturating rather than wrapping since levels can each hold up to max.
    Qty available(Side side, Price limit) const {
        constexpr Qty kMax = std::numeric_limits<Qty>::max();
        Qty total = 0;
        auto add_up = [&](const auto& book, auto crosses) {
            for (const auto& [p, lvl] : book) {
                if (!crosses(p)) break;
                total = lvl.qty > kMax - total ? kMax : total + lvl.qty;
            }
        };
        if (side == Side::Buy) add_up(asks_, [&](Price p) { return p <= limit; });
        else add_up(bids_, [&](Price p) { return p >= limit; });
        return total;
    }

    // book is updated before on_trade, so a throwing listener leaves it valid
    template <class Book, class Crosses>
    Qty match_against(Book& book, OrderId taker, Qty qty, Crosses crosses) {
        while (qty > 0 && !book.empty() && crosses(book.begin()->first)) {
            auto lvl = book.begin();
            Queue& q = lvl->second.orders;
            Order& o = q.front();
            const Qty f = std::min(qty, o.qty);
            const Trade trade{taker, o.id, lvl->first, f};
            o.qty -= f;
            lvl->second.qty -= f;
            qty -= f;
            if (o.qty == 0) {
                ids_.erase(o.id);
                q.pop_front();
                if (q.empty()) book.erase(lvl);
            }
            listener_.on_trade(trade);
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        Level& lvl = level(side, price);
        lvl.orders.push_back(Order{id, side, price, qty});
        lvl.qty += qty;
        ids_[id] = std::prev(lvl.orders.end());
    }

    void erase(typename std::unordered_map<OrderId, typename Queue::iterator>::iterator it) {
        const Order& o = *it->second;
        if (o.side == Side::Buy) erase_from(bids_, o.price, it->second);
        else erase_from(asks_, o.price, it->second);
        ids_.erase(it);
    }

    template <class Book>
    static void erase_from(Book& book, Price price, typename Queue::iterator pos) {
        auto lvl = book.find(price);
        lvl->second.qty -= pos->qty;
        lvl->second.orders.erase(pos);
        if (lvl->second.orders.empty()) book.erase(lvl);
    }

    Price min_price_;
    Price max_price_;
    std::size_t max_orders_;
    Listener listener_;

    std::map<Price, Level, std::greater<>> bids_;
    std::map<Price, Level> asks_;
    std::unordered_map<OrderId, typename Queue::iterator> ids_;
};

}  // namespace lob
