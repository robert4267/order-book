#pragma once

#include <ostream>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// gtest finds these by ADL, so failures print "book full" instead of raw bytes
inline void PrintTo(Status s, std::ostream* os) { *os << to_string(s); }

inline void PrintTo(const Trade& t, std::ostream* os) {
    *os << "{taker " << t.taker << ", maker " << t.maker << ", " << t.qty << " @ " << t.price
        << "}";
}

struct TradeLog {
    std::vector<Trade> trades;
    void on_trade(const Trade& t) { trades.push_back(t); }
};

}  // namespace lob
