// Text driver for the book. Reads commands from stdin, one per line:
//
//   A <id> <B|S> <price> <qty> [GTC|IOC|FOK|POST]    limit order, GTC by default
//   M <id> <B|S> <qty>            market order
//   X <id>                        cancel
//   R <id> <price> <qty>          modify (cancel/replace)
//   P [levels]                    print the top of the book
//
// Prices are integer ticks between 0 and 100000.

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

namespace {

struct Printer {
    void on_trade(const Trade& t) {
        std::printf("TRADE taker=%llu maker=%llu price=%lld qty=%llu\n",
                    static_cast<unsigned long long>(t.taker),
                    static_cast<unsigned long long>(t.maker), static_cast<long long>(t.price),
                    static_cast<unsigned long long>(t.qty));
    }
};

std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> out;
    constexpr std::string_view ws = " \t\r";
    for (std::size_t pos = line.find_first_not_of(ws); pos != std::string_view::npos;) {
        const std::size_t end = std::min(line.find_first_of(ws, pos), line.size());
        out.push_back(line.substr(pos, end - pos));
        pos = line.find_first_not_of(ws, end);
    }
    return out;
}

// The whole token has to be a number. from_chars doesn't take a leading '+'
// and rejects '-' for unsigned types, so "-5" can't sneak in as a huge qty.
template <class T>
std::optional<T> parse_num(std::string_view s) {
    T v{};
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<Tif> parse_tif(std::string_view s) {
    if (s == "GTC") return Tif::Gtc;
    if (s == "IOC") return Tif::Ioc;
    if (s == "FOK") return Tif::Fok;
    if (s == "POST") return Tif::PostOnly;
    return std::nullopt;
}

std::optional<Side> parse_side(std::string_view s) {
    if (s == "B" || s == "b") return Side::Buy;
    if (s == "S" || s == "s") return Side::Sell;
    return std::nullopt;
}

void print_book(const OrderBook<Printer>& book, std::size_t depth) {
    std::vector<std::pair<Price, Qty>> asks;
    book.for_each_level(Side::Sell, [&](Price p, Qty q) { asks.emplace_back(p, q); }, depth);
    std::printf("%10s %8s  %s\n", "bid qty", "price", "ask qty");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        std::printf("%10s %8lld  %llu\n", "", static_cast<long long>(it->first),
                    static_cast<unsigned long long>(it->second));
    }
    book.for_each_level(
        Side::Buy,
        [](Price p, Qty q) {
            std::printf("%10llu %8lld\n", static_cast<unsigned long long>(q),
                        static_cast<long long>(p));
        },
        depth);
}

}  // namespace

int main() {
    OrderBook<Printer> book(0, 100'000, 1'000'000);
    std::string line;
    int lineno = 0;

    while (std::getline(std::cin, line)) {
        ++lineno;
        const std::vector<std::string_view> tok = split(line);
        if (tok.empty() || tok[0][0] == '#') continue;

        bool ok = false;
        std::optional<Status> st;
        const std::string_view cmd = tok[0];

        if (cmd == "A" && (tok.size() == 5 || tok.size() == 6)) {
            const auto id = parse_num<OrderId>(tok[1]);
            const auto side = parse_side(tok[2]);
            const auto price = parse_num<Price>(tok[3]);
            const auto qty = parse_num<Qty>(tok[4]);
            const auto tif = tok.size() == 6 ? parse_tif(tok[5]) : std::optional<Tif>{Tif::Gtc};
            if ((ok = id && side && price && qty && tif))
                st = book.add(*id, *side, *price, *qty, *tif);
        } else if (cmd == "M" && tok.size() == 4) {
            const auto id = parse_num<OrderId>(tok[1]);
            const auto side = parse_side(tok[2]);
            const auto qty = parse_num<Qty>(tok[3]);
            if ((ok = id && side && qty)) st = book.market(*id, *side, *qty);
        } else if (cmd == "X" && tok.size() == 2) {
            const auto id = parse_num<OrderId>(tok[1]);
            if ((ok = id.has_value())) st = book.cancel(*id);
        } else if (cmd == "R" && tok.size() == 4) {
            const auto id = parse_num<OrderId>(tok[1]);
            const auto price = parse_num<Price>(tok[2]);
            const auto qty = parse_num<Qty>(tok[3]);
            if ((ok = id && price && qty)) st = book.modify(*id, *price, *qty);
        } else if (cmd == "P" && tok.size() <= 2) {
            const auto depth =
                tok.size() == 2 ? parse_num<std::size_t>(tok[1]) : std::optional<std::size_t>{5};
            if ((ok = depth.has_value())) print_book(book, *depth);
        }

        if (!ok) std::printf("line %d: can't parse '%s'\n", lineno, line.c_str());
        else if (st && *st != Status::Ok)
            std::printf("line %d: rejected (%s)\n", lineno, to_string(*st));
    }
}
