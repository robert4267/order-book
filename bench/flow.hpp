#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/types.hpp"

namespace lob::bench {

enum class OpType : std::uint8_t { Add, Market, Cancel, Modify };

struct Op {
    OpType type;
    Side side;
    OrderId id;
    Price price;
    Qty qty;
};

struct FlowConfig {
    std::size_t ops = 2'000'000;
    std::uint64_t seed = 42;
    Price min_price = 0;
    Price max_price = 100'000;
    // A guess at the shape of equity order flow, where most orders are
    // cancelled before they trade, not a measurement of any venue.
    int add_pct = 55;
    int cancel_pct = 35;
    int modify_pct = 5;  // the rest are market orders
    // below this many resting orders, cancels become adds (for deep books)
    std::size_t min_resting = 0;
    double distance_p = 0.15;  // geometric, distance of passive orders from mid
};

// What the generated flow looks like from the book's point of view, measured
// on the reference book while generating.
struct FlowStats {
    std::size_t resting_max = 0;
    double resting_avg = 0;
    double levels_avg = 0;  // non-empty levels, both sides, sampled
    std::size_t levels_touched = 0;
    double adds_trading = 0;  // fraction of adds that traded on arrival
    double spread_avg = 0;    // ticks, sampled when both sides are present
};

struct Flow {
    std::vector<Op> ops;
    FlowStats stats;
};

// Synthetic flow around a random-walk mid. Generated against the reference
// book so every cancel and modify hits a live order.
inline Flow make_flow(const FlowConfig& cfg) {
    struct Fills {
        std::vector<OrderId>* makers;
        bool* traded;
        void on_trade(const Trade& t) {
            makers->push_back(t.maker);
            *traded = true;
        }
    };

    struct Live {
        OrderId id;
        Side side;
        Price price;
    };

    std::vector<OrderId> makers;
    bool traded = false;
    MapOrderBook<Fills> ref(cfg.min_price, cfg.max_price, SIZE_MAX, Fills{&makers, &traded});

    std::vector<Live> live;
    std::unordered_map<OrderId, std::size_t> pos;         // id -> index in live
    std::unordered_map<Price, std::size_t> per_level[2];  // live orders per price, by side
    auto level_of = [&](const Live& l) -> auto& { return per_level[l.side == Side::Sell]; };
    auto drop = [&](OrderId id) {
        const auto it = pos.find(id);
        if (it == pos.end()) return;
        const std::size_t i = it->second;
        pos.erase(it);
        auto& lv = level_of(live[i]);
        if (--lv[live[i].price] == 0) lv.erase(live[i].price);
        if (i + 1 != live.size()) {
            live[i] = live.back();
            pos[live[i].id] = i;
        }
        live.pop_back();
    };
    auto keep_if_resting = [&](const Live& l) {
        if (!ref.order_qty(l.id)) return;
        pos[l.id] = live.size();
        live.push_back(l);
        ++level_of(l)[l.price];
    };
    auto settle_fills = [&] {
        for (OrderId m : makers) {
            if (!ref.order_qty(m)) drop(m);
        }
        makers.clear();
    };

    std::mt19937_64 rng(cfg.seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::geometric_distribution<int> distance(cfg.distance_p);
    std::uniform_int_distribution<Qty> lot(1, 500);

    Flow flow;
    flow.ops.reserve(cfg.ops);
    std::vector<bool> touched(static_cast<std::size_t>(cfg.max_price - cfg.min_price) + 1);
    std::size_t adds = 0, adds_traded = 0, samples = 0, spread_samples = 0;
    double resting_sum = 0, levels_sum = 0, spread_sum = 0;

    const Price center = (cfg.min_price + cfg.max_price) / 2;
    Price mid = center;
    OrderId next_id = 1;

    auto clamp = [&](Price p) { return std::clamp(p, cfg.min_price, cfg.max_price); };
    auto random_side = [&] { return rng() & 1 ? Side::Buy : Side::Sell; };
    auto touch = [&](Price p) { touched[static_cast<std::size_t>(p - cfg.min_price)] = true; };
    auto must = [](Status s) {
        if (s != Status::Ok) throw std::logic_error("flow generator out of sync with ref book");
    };

    while (flow.ops.size() < cfg.ops) {
        if (pct(rng) < 10) mid = clamp(mid + (rng() & 1 ? 1 : -1));
        // keep the walk from drifting off to one end over millions of steps
        if (mid > center + 5000 || mid < center - 5000) mid = center;

        traded = false;
        const int r = pct(rng);
        const bool refill = live.size() < cfg.min_resting && r < cfg.add_pct + cfg.cancel_pct;
        if (live.empty() || r < cfg.add_pct || refill) {
            const Side side = random_side();
            const Price d = std::min(distance(rng), 2000);
            const bool aggressive = pct(rng) < 10;
            Price price;
            if (side == Side::Buy) price = aggressive ? mid + d % 5 : mid - 1 - d;
            else price = aggressive ? mid - d % 5 : mid + 1 + d;
            price = clamp(price);

            const Op op{OpType::Add, side, next_id++, price, lot(rng)};
            must(ref.add(op.id, op.side, op.price, op.qty));
            flow.ops.push_back(op);
            settle_fills();
            keep_if_resting({op.id, side, price});
            touch(price);
            ++adds;
            adds_traded += traded;
        } else if (r < cfg.add_pct + cfg.cancel_pct) {
            const Live l = live[rng() % live.size()];
            must(ref.cancel(l.id));
            flow.ops.push_back({OpType::Cancel, l.side, l.id, 0, 0});
            drop(l.id);
        } else if (r < cfg.add_pct + cfg.cancel_pct + cfg.modify_pct) {
            const Live l = live[rng() % live.size()];
            Op op{OpType::Modify, l.side, l.id, l.price, 0};
            if (pct(rng) < 50) {
                op.qty = lot(rng) / 4 + 1;
            } else {
                const Price shift = (rng() & 1 ? 1 : -1) * static_cast<Price>(1 + rng() % 3);
                op.price = clamp(l.price + shift);
                op.qty = lot(rng);
            }
            must(ref.modify(op.id, op.price, op.qty));
            flow.ops.push_back(op);
            drop(l.id);
            settle_fills();
            keep_if_resting({l.id, l.side, op.price});
            touch(op.price);
        } else {
            const Op op{OpType::Market, random_side(), next_id++, 0, lot(rng)};
            must(ref.market(op.id, op.side, op.qty));
            flow.ops.push_back(op);
            settle_fills();
        }

        if (flow.ops.size() % 64 == 0) {
            ++samples;
            resting_sum += static_cast<double>(ref.order_count());
            levels_sum += static_cast<double>(per_level[0].size() + per_level[1].size());
            if (const auto bid = ref.best_bid(), ask = ref.best_ask(); bid && ask) {
                ++spread_samples;
                spread_sum += static_cast<double>(*ask - *bid);
            }
        }
        flow.stats.resting_max = std::max(flow.stats.resting_max, ref.order_count());
    }

    FlowStats& s = flow.stats;
    s.resting_avg = samples ? resting_sum / static_cast<double>(samples) : 0;
    s.levels_avg = samples ? levels_sum / static_cast<double>(samples) : 0;
    s.spread_avg = spread_samples ? spread_sum / static_cast<double>(spread_samples) : 0;
    s.levels_touched = static_cast<std::size_t>(std::count(touched.begin(), touched.end(), true));
    s.adds_trading = adds ? static_cast<double>(adds_traded) / static_cast<double>(adds) : 0;
    return flow;
}

}  // namespace lob::bench
