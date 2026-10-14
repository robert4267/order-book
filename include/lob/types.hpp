#pragma once

#include <cstddef>
#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // integer ticks, never floating point
using Qty = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

// Time in force for limit orders.
//   Gtc       rests whatever doesn't fill immediately
//   Ioc       fills what it can, the rest is cancelled
//   Fok       fills completely right away or not at all
//   PostOnly  rests without trading; rejected if it would cross
enum class Tif : std::uint8_t { Gtc, Ioc, Fok, PostOnly };

enum class [[nodiscard]] Status : std::uint8_t {
    Ok,
    DuplicateId,
    UnknownId,
    BadPrice,
    BadQty,
    BookFull,
    Killed,      // FOK that couldn't fill in full, nothing traded
    WouldCross,  // post-only that would have taken liquidity
};

constexpr const char* to_string(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::DuplicateId: return "duplicate id";
        case Status::UnknownId: return "unknown id";
        case Status::BadPrice: return "bad price";
        case Status::BadQty: return "bad qty";
        case Status::BookFull: return "book full";
        case Status::Killed: return "killed";
        case Status::WouldCross: return "would cross";
    }
    return "?";
}

// For tables indexed by status. Keep it in step with the last enumerator.
inline constexpr std::size_t kStatusCount = static_cast<std::size_t>(Status::WouldCross) + 1;

struct Trade {
    OrderId taker;
    OrderId maker;
    Price price;  // always the resting order's price
    Qty qty;

    bool operator==(const Trade&) const = default;
};

// A resting order as seen from outside the book.
struct OrderInfo {
    Side side;
    Price price;
    Qty qty;

    bool operator==(const OrderInfo&) const = default;
};

struct NullListener {
    void on_trade(const Trade&) {}
};

}  // namespace lob
