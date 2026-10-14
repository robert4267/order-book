#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lob/itch.hpp"
#include "lob/itch_book.hpp"

using namespace lob;
using namespace lob::itch;
namespace enc = lob::itch::encode;

namespace {

// Raw big-endian bytes, written independently of the encoder so the layout
// tests don't just check the encoder against itself.
std::string be(std::uint64_t v, int bytes) {
    std::string s;
    for (int i = bytes - 1; i >= 0; --i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    return s;
}

struct Recorder {
    std::string types;
    std::vector<SystemEvent> sys;
    std::vector<StockDirectory> dir;
    std::vector<AddOrder> adds;
    std::vector<OrderExecuted> execs;
    std::vector<OrderCancel> cancels;
    std::vector<OrderDelete> deletes;
    std::vector<OrderReplace> replaces;

    void on_message(char t) { types.push_back(t); }
    void on(const SystemEvent& m) { sys.push_back(m); }
    void on(const StockDirectory& m) { dir.push_back(m); }
    void on(const AddOrder& m) { adds.push_back(m); }
    void on(const OrderExecuted& m) { execs.push_back(m); }
    void on(const OrderCancel& m) { cancels.push_back(m); }
    void on(const OrderDelete& m) { deletes.push_back(m); }
    void on(const OrderReplace& m) { replaces.push_back(m); }
};

// Parses from an exactly sized heap copy so ASan catches any read past the end.
template <class H>
Result parse_exact(std::string_view bytes, H& h) {
    const std::vector<char> copy(bytes.begin(), bytes.end());
    return parse(copy.data(), copy.size(), h);
}

std::string sample_stream() {
    std::string s;
    enc::system_event(s, 'O', 1);
    enc::stock_directory(s, 7, "AAPL", 2);
    enc::add_order(s, 7, 1, 'B', 100, "AAPL", 1'500'000, {}, 3);
    enc::add_order(s, 7, 2, 'S', 200, "AAPL", 1'500'100, "GSCO", 4);
    enc::executed(s, 7, 1, 40, 900, 5);
    enc::executed_with_price(s, 7, 2, 50, 901, false, 1'500'050, 6);
    enc::cancel(s, 7, 2, 25, 7);
    enc::trade(s, 7, 'B', 300, "AAPL", 1'500'000, 902, 8);
    enc::replace(s, 7, 1, 3, 70, 1'499'900, 9);
    enc::del(s, 7, 3, 10);
    return s;
}

}  // namespace

TEST(Itch, ByteswapFallbackMatches) {
    EXPECT_EQ(detail::byteswap(std::uint16_t{0x1234}), 0x3412);
    EXPECT_EQ(detail::byteswap(std::uint32_t{0x12345678}), 0x78563412u);
    EXPECT_EQ(detail::byteswap(std::uint64_t{0x0102030405060708}), 0x0807060504030201u);
}

TEST(Itch, AddOrderMatchesSpecLayout) {
    std::string raw = be(36, 2);
    raw += 'A';
    raw += be(7, 2) + be(0x0BAD, 2) + be(0x010203040506, 6);
    raw += be(0x0102030405060708, 8);
    raw += 'B';
    raw += be(100, 4);
    raw += "AAPL    ";
    raw += be(1'501'234, 4);
    ASSERT_EQ(raw.size(), 38u);

    std::string encoded;
    enc::add_order(encoded, 7, 0x0102030405060708, 'B', 100, "AAPL", 1'501'234, {}, 0x010203040506);
    encoded[3 + 2] = '\x0B';  // encoder writes tracking 0
    encoded[3 + 3] = '\xAD';
    EXPECT_EQ(encoded, raw);

    Recorder r;
    const Result res = parse_exact(raw, r);
    EXPECT_EQ(res.error, Error::None);
    EXPECT_EQ(res.consumed, raw.size());
    ASSERT_EQ(r.adds.size(), 1u);
    const AddOrder& a = r.adds[0];
    EXPECT_EQ(a.h.type, 'A');
    EXPECT_EQ(a.h.locate, 7);
    EXPECT_EQ(a.h.tracking, 0x0BAD);
    EXPECT_EQ(a.h.timestamp, 0x010203040506u);
    EXPECT_EQ(a.ref, 0x0102030405060708u);
    EXPECT_EQ(a.side, 'B');
    EXPECT_EQ(a.shares, 100u);
    EXPECT_EQ(trim(a.stock), "AAPL");
    EXPECT_EQ(a.price, 1'501'234u);
    EXPECT_EQ(std::string_view(a.mpid.data(), 4), "    ");
}

TEST(Itch, ReplaceMatchesSpecLayout) {
    std::string raw = be(35, 2) + "U" + be(3, 2) + be(0, 2) + be(99, 6);
    raw += be(11, 8) + be(12, 8) + be(500, 4) + be(2'000'000, 4);
    Recorder r;
    ASSERT_EQ(parse_exact(raw, r).error, Error::None);
    ASSERT_EQ(r.replaces.size(), 1u);
    EXPECT_EQ(r.replaces[0].old_ref, 11u);
    EXPECT_EQ(r.replaces[0].new_ref, 12u);
    EXPECT_EQ(r.replaces[0].shares, 500u);
    EXPECT_EQ(r.replaces[0].price, 2'000'000u);
}

// The remaining handled types, each built from the spec's field offsets.
// Header fields are distinct per message so a shifted field can't pass.
namespace {

std::string header(char type, std::uint16_t len, std::uint16_t locate, std::uint64_t ts) {
    return be(len, 2) + type + be(locate, 2) + be(0x4242, 2) + be(ts, 6);
}

template <class M>
void expect_header(const M& m, char type, std::uint16_t locate, std::uint64_t ts) {
    EXPECT_EQ(m.h.type, type);
    EXPECT_EQ(m.h.locate, locate);
    EXPECT_EQ(m.h.tracking, 0x4242);
    EXPECT_EQ(m.h.timestamp, ts);
}

}  // namespace

TEST(Itch, SystemEventMatchesSpecLayout) {
    const std::string raw = header('S', 12, 0, 0x0A0B0C0D0E0F) + "Q";
    Recorder r;
    ASSERT_EQ(parse_exact(raw, r).error, Error::None);
    ASSERT_EQ(r.sys.size(), 1u);
    expect_header(r.sys[0], 'S', 0, 0x0A0B0C0D0E0Fu);
    EXPECT_EQ(r.sys[0].event, 'Q');
}

TEST(Itch, StockDirectoryMatchesSpecLayout) {
    // stock@11, market category@19, financial status@20, round lot@21, then
    // 14 bytes of flags the decoder doesn't need
    std::string raw = header('R', 39, 513, 77) + "MSFT    " + "Q" + "N" + be(100, 4);
    raw += std::string(14, 'x');
    ASSERT_EQ(raw.size(), 41u);
    Recorder r;
    ASSERT_EQ(parse_exact(raw, r).error, Error::None);
    ASSERT_EQ(r.dir.size(), 1u);
    expect_header(r.dir[0], 'R', 513, 77);
    EXPECT_EQ(trim(r.dir[0].stock), "MSFT");
    EXPECT_EQ(r.dir[0].market_category, 'Q');
    EXPECT_EQ(r.dir[0].financial_status, 'N');
    EXPECT_EQ(r.dir[0].round_lot, 100u);
}

TEST(Itch, AddWithMpidMatchesSpecLayout) {
    std::string raw = header('F', 40, 9, 1234) + be(0xFEDCBA9876543210, 8) + "S" + be(250, 4);
    raw += "QQQ     " + be(4'321'000, 4) + "GSCO";
    ASSERT_EQ(raw.size(), 42u);
    Recorder r;
    ASSERT_EQ(parse_exact(raw, r).error, Error::None);
    ASSERT_EQ(r.adds.size(), 1u);
    const AddOrder& a = r.adds[0];
    expect_header(a, 'F', 9, 1234);
    EXPECT_EQ(a.ref, 0xFEDCBA9876543210u);
    EXPECT_EQ(a.side, 'S');
    EXPECT_EQ(a.shares, 250u);
    EXPECT_EQ(trim(a.stock), "QQQ");
    EXPECT_EQ(a.price, 4'321'000u);
    EXPECT_EQ(std::string_view(a.mpid.data(), 4), "GSCO");
}

TEST(Itch, ExecutedMatchesSpecLayout) {
    const std::string raw = header('E', 31, 5, 99) + be(0x1122334455667788, 8) + be(0xA1B2C3D4, 4) +
                            be(0x0102030405060708, 8);
    Recorder r;
    ASSERT_EQ(parse_exact(raw, r).error, Error::None);
    ASSERT_EQ(r.execs.size(), 1u);
    expect_header(r.execs[0], 'E', 5, 99);
    EXPECT_EQ(r.execs[0].ref, 0x1122334455667788u);
    EXPECT_EQ(r.execs[0].shares, 0xA1B2C3D4u);
    EXPECT_EQ(r.execs[0].match, 0x0102030405060708u);
}

TEST(Itch, ExecutedWithPriceMatchesSpecLayout) {
    const std::string raw =
        header('C', 36, 6, 100) + be(31, 8) + be(40, 4) + be(55, 8) + "Y" + be(1'234'567, 4);
    Recorder r;
    ASSERT_EQ(parse_exact(raw, r).error, Error::None);
    ASSERT_EQ(r.execs.size(), 1u);
    const OrderExecuted& e = r.execs[0];
    expect_header(e, 'C', 6, 100);
    EXPECT_EQ(e.ref, 31u);
    EXPECT_EQ(e.shares, 40u);
    EXPECT_EQ(e.match, 55u);
    EXPECT_TRUE(e.printable);
    EXPECT_EQ(e.price, 1'234'567u);
}

TEST(Itch, CancelAndDeleteMatchSpecLayout) {
    const std::string raw = header('X', 23, 3, 1) + be(0x0A0B0C0D, 8) + be(17, 4) +
                            header('D', 19, 4, 2) + be(0x0E0F1011, 8);
    Recorder r;
    ASSERT_EQ(parse_exact(raw, r).error, Error::None);
    ASSERT_EQ(r.cancels.size(), 1u);
    ASSERT_EQ(r.deletes.size(), 1u);
    expect_header(r.cancels[0], 'X', 3, 1);
    EXPECT_EQ(r.cancels[0].ref, 0x0A0B0C0Du);
    EXPECT_EQ(r.cancels[0].shares, 17u);
    expect_header(r.deletes[0], 'D', 4, 2);
    EXPECT_EQ(r.deletes[0].ref, 0x0E0F1011u);
}

TEST(Itch, EncodedFramesHaveSpecLengths) {
    auto check = [](char type, auto write) {
        std::string s;
        write(s);
        ASSERT_GE(s.size(), 3u) << type;
        EXPECT_EQ(s[2], type);
        EXPECT_EQ(s.size(), message_size(type) + 2) << type;
        EXPECT_EQ((static_cast<unsigned char>(s[0]) << 8) | static_cast<unsigned char>(s[1]),
                  static_cast<int>(message_size(type)));
    };
    check('S', [](std::string& s) { enc::system_event(s, 'O'); });
    check('R', [](std::string& s) { enc::stock_directory(s, 1, "AAPL"); });
    check('A', [](std::string& s) { enc::add_order(s, 1, 1, 'B', 1, "AAPL", 1); });
    check('F', [](std::string& s) { enc::add_order(s, 1, 1, 'B', 1, "AAPL", 1, "GSCO"); });
    check('E', [](std::string& s) { enc::executed(s, 1, 1, 1, 1); });
    check('C', [](std::string& s) { enc::executed_with_price(s, 1, 1, 1, 1, true, 1); });
    check('X', [](std::string& s) { enc::cancel(s, 1, 1, 1); });
    check('D', [](std::string& s) { enc::del(s, 1, 1); });
    check('U', [](std::string& s) { enc::replace(s, 1, 1, 2, 1, 1); });
    check('P', [](std::string& s) { enc::trade(s, 1, 'B', 1, "AAPL", 1, 1); });
}

TEST(Itch, DecodesEveryHandledType) {
    const std::string s = sample_stream();
    Recorder r;
    const Result res = parse_exact(s, r);
    EXPECT_EQ(res.error, Error::None);
    EXPECT_EQ(res.consumed, s.size());
    EXPECT_EQ(res.messages, 10u);
    EXPECT_EQ(r.types, "SRAFECXPUD");

    ASSERT_EQ(r.sys.size(), 1u);
    EXPECT_EQ(r.sys[0].event, 'O');
    EXPECT_EQ(r.sys[0].h.timestamp, 1u);

    ASSERT_EQ(r.dir.size(), 1u);
    EXPECT_EQ(trim(r.dir[0].stock), "AAPL");
    EXPECT_EQ(r.dir[0].h.locate, 7);
    EXPECT_EQ(r.dir[0].round_lot, 100u);

    ASSERT_EQ(r.adds.size(), 2u);
    EXPECT_EQ(r.adds[1].h.type, 'F');
    EXPECT_EQ(r.adds[1].side, 'S');
    EXPECT_EQ(r.adds[1].shares, 200u);
    EXPECT_EQ(std::string_view(r.adds[1].mpid.data(), 4), "GSCO");

    ASSERT_EQ(r.execs.size(), 2u);
    EXPECT_EQ(r.execs[0].h.type, 'E');
    EXPECT_EQ(r.execs[0].ref, 1u);
    EXPECT_EQ(r.execs[0].shares, 40u);
    EXPECT_EQ(r.execs[0].match, 900u);
    EXPECT_EQ(r.execs[1].h.type, 'C');
    EXPECT_FALSE(r.execs[1].printable);
    EXPECT_EQ(r.execs[1].price, 1'500'050u);

    ASSERT_EQ(r.cancels.size(), 1u);
    EXPECT_EQ(r.cancels[0].shares, 25u);
    ASSERT_EQ(r.deletes.size(), 1u);
    EXPECT_EQ(r.deletes[0].ref, 3u);
    EXPECT_EQ(r.deletes[0].h.timestamp, 10u);
}

TEST(Itch, SkipsUnknownAndOversizedFrames) {
    std::string s = be(5, 2) + "Zabcd";  // not an ITCH type
    std::string longer;
    enc::del(longer, 1, 42);
    longer[1] = static_cast<char>(longer[1] + 3);  // claim 3 extra trailing bytes
    longer += "xyz";
    s += longer;
    s += be(0, 2);  // end of session
    enc::system_event(s, 'C');

    Recorder r;
    const Result res = parse_exact(s, r);
    EXPECT_EQ(res.error, Error::None);
    EXPECT_EQ(res.consumed, s.size());
    EXPECT_EQ(r.types, "ZDS");
    ASSERT_EQ(r.deletes.size(), 1u);
    EXPECT_EQ(r.deletes[0].ref, 42u);
}

TEST(Itch, ShortFrameIsBadLength) {
    std::string s;
    enc::system_event(s, 'O');
    const std::size_t good = s.size();
    s += be(20, 2) + "A" + std::string(19, '\0');  // an add needs 36

    Recorder r;
    const Result res = parse_exact(s, r);
    EXPECT_EQ(res.error, Error::BadLength);
    EXPECT_EQ(res.consumed, good);
    EXPECT_EQ(res.messages, 1u);
    EXPECT_TRUE(r.adds.empty());
}

TEST(Itch, EveryTruncationStopsAtAFrameBoundary) {
    const std::string s = sample_stream();
    std::vector<std::size_t> ends;
    for (std::size_t at = 0; at < s.size();) {
        at +=
            2 + ((static_cast<unsigned char>(s[at]) << 8) | static_cast<unsigned char>(s[at + 1]));
        ends.push_back(at);
    }
    for (std::size_t cut = 0; cut <= s.size(); ++cut) {
        Recorder r;
        const Result res = parse_exact(std::string_view(s).substr(0, cut), r);
        std::size_t whole = 0, msgs = 0;
        for (std::size_t e : ends)
            if (e <= cut) whole = e, ++msgs;
        EXPECT_EQ(res.consumed, whole) << cut;
        EXPECT_EQ(res.messages, msgs) << cut;
        EXPECT_EQ(res.error, whole == cut ? Error::None : Error::Truncated) << cut;
    }
}

TEST(Itch, ChunkedParsingMatchesWholeBuffer) {
    const std::string s = sample_stream();
    Recorder whole;
    ASSERT_EQ(parse_exact(s, whole).error, Error::None);

    for (std::size_t split = 0; split <= s.size(); ++split) {
        Recorder r;
        const Result first = parse_exact(std::string_view(s).substr(0, split), r);
        const std::string rest = s.substr(first.consumed);
        const Result second = parse_exact(rest, r);
        EXPECT_EQ(second.error, Error::None);
        EXPECT_EQ(first.messages + second.messages, 10u);
        EXPECT_EQ(r.types, whole.types) << split;
    }
}

TEST(Itch, GarbageNeverReadsOutOfBounds) {
    std::mt19937_64 rng(7);
    BookConfig cfg;
    cfg.max_levels = 256;
    cfg.max_orders = 16;
    BookBuilder all(cfg);
    for (int iter = 0; iter < 2000; ++iter) {
        std::string junk(rng() % 300, '\0');
        for (char& c : junk) c = static_cast<char>(rng());
        // mostly plausible lengths and types, so the decoders actually run
        for (std::size_t at = 0; at + 3 <= junk.size() && rng() % 4 != 0;) {
            const char types[] = "SRAFECXDUP";
            const char t = types[rng() % 10];
            const std::size_t len = message_size(t) + (rng() % 8 == 0 ? rng() % 4 : 0);
            junk[at] = static_cast<char>(len >> 8);
            junk[at + 1] = static_cast<char>(len & 0xFF);
            junk[at + 2] = t;
            if (at + 4 < junk.size()) {  // keep locates small so books get reused
                junk[at + 3] = 0;
                junk[at + 4] = static_cast<char>(rng() % 4);
            }
            at += 2 + len;
        }
        Recorder r;
        const Result res = parse_exact(junk, r);
        EXPECT_LE(res.consumed, junk.size());
        const Result res2 = parse_exact(junk, all);
        EXPECT_EQ(res.consumed, res2.consumed);
    }
    all.for_each_book([](std::uint16_t, const BookBuilder::Book& b) { EXPECT_TRUE(b.validate()); });
}

// Book building

namespace {

struct Feed {
    std::string bytes;
    BookBuilder bb;

    static BookConfig small() {
        BookConfig c;
        c.max_levels = 1u << 18;
        c.max_orders = 1u << 12;
        return c;
    }

    explicit Feed(BookConfig cfg = small(), std::string_view symbol = "AAPL") : bb(cfg, symbol) {
        enc::stock_directory(bytes, 1, "AAPL");
        enc::stock_directory(bytes, 2, "MSFT");
        flush();
    }
    void flush() {
        const Result r = parse_exact(bytes, bb);
        ASSERT_EQ(r.error, Error::None);
        bytes.clear();
    }
    BookBuilder::Book& book() { return *bb.book(1); }
};

constexpr std::uint32_t usd(double d) { return static_cast<std::uint32_t>(d * 10000 + 0.5); }

}  // namespace

TEST(ItchBook, AddsRestWithoutMatchingAndFiltersBySymbol) {
    Feed f;
    enc::add_order(f.bytes, 1, 10, 'B', 100, "AAPL", usd(150.00));
    enc::add_order(f.bytes, 1, 11, 'S', 300, "AAPL", usd(150.01), "GSCO");
    enc::add_order(f.bytes, 2, 12, 'B', 999, "MSFT", usd(150.05));
    enc::executed(f.bytes, 2, 12, 1, 1);
    f.flush();

    EXPECT_EQ(f.bb.target(), 1);
    EXPECT_EQ(f.bb.book(2), nullptr);
    EXPECT_EQ(f.book().best_bid(), Price{usd(150.00)});
    EXPECT_EQ(f.book().best_ask(), Price{usd(150.01)});
    EXPECT_EQ(f.book().order(11), (OrderInfo{Side::Sell, usd(150.01), 300}));
    EXPECT_EQ(f.bb.stats().adds, 2u);
    EXPECT_EQ(f.bb.stats().executes, 0u);
    EXPECT_TRUE(f.book().validate());
}

TEST(ItchBook, PartialExecuteAndCancelKeepPriority) {
    Feed f;
    enc::add_order(f.bytes, 1, 1, 'B', 100, "AAPL", usd(10));
    enc::add_order(f.bytes, 1, 2, 'B', 100, "AAPL", usd(10));
    enc::executed(f.bytes, 1, 1, 30, 1);
    enc::cancel(f.bytes, 1, 1, 20);
    f.flush();

    EXPECT_EQ(f.book().order_qty(1), 50u);
    EXPECT_EQ(f.book().depth_at(Side::Buy, usd(10)), 150u);
    // a sell for exactly order 1's remainder must take order 1, not order 2
    ASSERT_EQ(f.book().market(99, Side::Sell, 50), Status::Ok);
    EXPECT_FALSE(f.book().order(1));
    EXPECT_EQ(f.book().order_qty(2), 100u);
    EXPECT_TRUE(f.book().validate());
}

TEST(ItchBook, ExecuteWithPriceReducesLikeExecute) {
    Feed f;
    enc::add_order(f.bytes, 1, 1, 'S', 100, "AAPL", usd(10));
    enc::executed_with_price(f.bytes, 1, 1, 60, 1, true, usd(9.99));
    f.flush();
    EXPECT_EQ(f.book().order_qty(1), 40u);
    EXPECT_EQ(f.bb.stats().executes, 1u);
}

TEST(ItchBook, FullExecuteOrCancelRemovesOrder) {
    Feed f;
    enc::add_order(f.bytes, 1, 1, 'B', 100, "AAPL", usd(10));
    enc::add_order(f.bytes, 1, 2, 'B', 100, "AAPL", usd(9));
    enc::executed(f.bytes, 1, 1, 100, 1);
    enc::cancel(f.bytes, 1, 2, 150);  // more than resting
    f.flush();
    EXPECT_EQ(f.book().order_count(), 0u);
    EXPECT_FALSE(f.book().best_bid());
    EXPECT_EQ(f.bb.stats().over_reduce, 1u);
    EXPECT_TRUE(f.book().validate());
}

TEST(ItchBook, DeleteRemovesAndUnknownRefsAreCounted) {
    Feed f;
    enc::add_order(f.bytes, 1, 1, 'S', 100, "AAPL", usd(10));
    enc::del(f.bytes, 1, 1);
    enc::del(f.bytes, 1, 1);
    enc::executed(f.bytes, 1, 77, 10, 1);
    enc::cancel(f.bytes, 1, 78, 10);
    enc::replace(f.bytes, 1, 79, 80, 10, usd(10));
    f.flush();
    EXPECT_EQ(f.book().order_count(), 0u);
    EXPECT_EQ(f.bb.stats().unknown_ref, 4u);
    EXPECT_FALSE(f.book().order(80));  // side of a replaced unknown order isn't known
}

TEST(ItchBook, ReplaceKeepsSideAndLosesPriority) {
    Feed f;
    enc::add_order(f.bytes, 1, 1, 'S', 100, "AAPL", usd(10));
    enc::add_order(f.bytes, 1, 2, 'S', 100, "AAPL", usd(10));
    enc::replace(f.bytes, 1, 1, 3, 100, usd(10));  // same price and size, new ref
    enc::replace(f.bytes, 1, 2, 4, 50, usd(10.5));
    f.flush();

    EXPECT_FALSE(f.book().order(1));
    EXPECT_FALSE(f.book().order(2));
    EXPECT_EQ(f.book().order(3), (OrderInfo{Side::Sell, usd(10), 100}));
    EXPECT_EQ(f.book().order(4), (OrderInfo{Side::Sell, usd(10.5), 50}));
    EXPECT_EQ(f.bb.stats().replaces, 2u);

    enc::add_order(f.bytes, 1, 5, 'S', 100, "AAPL", usd(10));
    enc::replace(f.bytes, 1, 3, 6, 100, usd(10));  // goes behind order 5
    f.flush();
    ASSERT_EQ(f.book().market(99, Side::Buy, 100), Status::Ok);
    EXPECT_FALSE(f.book().order(5));
    EXPECT_EQ(f.book().order_qty(6), 100u);
    EXPECT_TRUE(f.book().validate());
}

TEST(ItchBook, CrossingAddIsRefusedNotMatched) {
    Feed f;
    enc::add_order(f.bytes, 1, 1, 'S', 100, "AAPL", usd(10));
    enc::add_order(f.bytes, 1, 2, 'B', 100, "AAPL", usd(10));
    enc::executed(f.bytes, 1, 2, 10, 1);
    f.flush();
    EXPECT_EQ(f.book().order_qty(1), 100u);  // no phantom trade
    EXPECT_FALSE(f.book().order(2));
    EXPECT_EQ(f.bb.stats().crossed, 1u);
    EXPECT_EQ(f.bb.stats().unknown_ref, 1u);
}

TEST(ItchBook, OutOfRangeOffTickDuplicateAndFullAreCounted) {
    BookConfig cfg = Feed::small();
    cfg.tick = 100;
    cfg.window = 0.1;
    cfg.max_orders = 2;
    Feed f(cfg);
    enc::add_order(f.bytes, 1, 1, 'B', 100, "AAPL", usd(100));     // window $90 - $110
    enc::add_order(f.bytes, 1, 2, 'B', 100, "AAPL", usd(120));     // out of range
    enc::add_order(f.bytes, 1, 3, 'B', 100, "AAPL", usd(99.995));  // sub-penny
    enc::add_order(f.bytes, 1, 1, 'B', 100, "AAPL", usd(99));      // duplicate ref
    enc::add_order(f.bytes, 1, 4, 'B', 0, "AAPL", usd(99));        // zero shares
    enc::add_order(f.bytes, 1, 5, 'B', 100, "AAPL", usd(98));
    enc::add_order(f.bytes, 1, 6, 'B', 100, "AAPL", usd(97));  // full
    f.flush();

    const BookStats& s = f.bb.stats();
    EXPECT_EQ(s.out_of_range, 1u);
    EXPECT_EQ(s.off_tick, 1u);
    EXPECT_EQ(s.duplicate, 1u);
    EXPECT_EQ(s.bad_qty, 1u);
    EXPECT_EQ(s.full, 1u);
    EXPECT_EQ(s.skipped(), 5u);
    EXPECT_EQ(f.book().order_count(), 2u);
    EXPECT_EQ(f.bb.to_itch(*f.book().best_bid()), usd(100));
}

TEST(ItchBook, ExplicitCentreOverridesFirstPrice) {
    BookConfig cfg = Feed::small();
    cfg.window = 0.1;
    cfg.centre = usd(100);
    Feed f(cfg);
    enc::add_order(f.bytes, 1, 1, 'S', 100, "AAPL", usd(105));  // stub quote far from centre
    enc::add_order(f.bytes, 1, 2, 'B', 100, "AAPL", usd(91));
    f.flush();
    EXPECT_EQ(f.book().min_price(), Price{usd(90)});
    EXPECT_EQ(f.book().max_price(), Price{usd(110)});
    EXPECT_EQ(f.book().order_count(), 2u);
}

TEST(ItchBook, FindsSymbolWithoutDirectory) {
    BookBuilder bb(Feed::small(), "TSLA");
    std::string s;
    enc::add_order(s, 9, 1, 'B', 100, "TSLA", usd(200));
    enc::add_order(s, 8, 2, 'B', 100, "AAPL", usd(150));
    ASSERT_EQ(parse_exact(s, bb).error, Error::None);
    EXPECT_EQ(bb.target(), 9);
    EXPECT_EQ(bb.name(9), "TSLA");
    EXPECT_EQ(bb.book(8), nullptr);
}

// Random feed against a plain map of resting orders: the book's levels and
// orders must match exactly after every batch.
TEST(ItchBook, MatchesReferenceOnRandomFeed) {
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        std::mt19937_64 rng(seed);
        struct Ref {
            Side side;
            Price price;
            Qty qty;
        };
        std::map<std::uint64_t, Ref> live;
        std::uint64_t next = 1;
        BookConfig cfg = Feed::small();
        cfg.window = 0.01;  // $99 - $101, keeps validate() cheap
        Feed f(cfg);
        enc::add_order(f.bytes, 1, next, 'B', 100, "AAPL", 1'000'000);
        live[next++] = {Side::Buy, 1'000'000, 100};

        auto best = [&](Side side) {
            std::optional<Price> b;
            for (const auto& [id, o] : live)
                if (o.side == side && (!b || (side == Side::Buy ? o.price > *b : o.price < *b)))
                    b = o.price;
            return b;
        };
        auto pick = [&] {
            auto it = live.begin();
            std::advance(it, static_cast<long>(rng() % live.size()));
            return it;
        };
        auto passive = [&](Side side) {
            const auto opp = best(opposite(side));
            Price p = 1'000'000 + static_cast<Price>(rng() % 2001) - 1000;
            if (opp) p = side == Side::Buy ? std::min(p, *opp - 1) : std::max(p, *opp + 1);
            return static_cast<std::uint32_t>(p);
        };

        for (int step = 0; step < 4000; ++step) {
            const auto r = rng() % 100;
            if (live.empty() || r < 45) {
                const Side side = rng() % 2 ? Side::Buy : Side::Sell;
                const std::uint32_t px = passive(side);
                const auto qty = static_cast<std::uint32_t>(1 + rng() % 500);
                enc::add_order(f.bytes, 1, next, side == Side::Buy ? 'B' : 'S', qty, "AAPL", px);
                live[next++] = {side, px, qty};
            } else if (r < 75) {
                auto it = pick();
                enc::del(f.bytes, 1, it->first);
                live.erase(it);
            } else if (r < 85) {
                auto it = pick();
                const Ref o = it->second;
                const std::uint32_t px = passive(o.side);
                const auto qty = static_cast<std::uint32_t>(1 + rng() % 500);
                enc::replace(f.bytes, 1, it->first, next, qty, px);
                live.erase(it);
                live[next++] = {o.side, px, qty};
            } else {
                auto it = pick();
                const auto qty = static_cast<std::uint32_t>(1 + rng() % it->second.qty);
                if (r < 95) enc::executed(f.bytes, 1, it->first, qty, 1);
                else enc::cancel(f.bytes, 1, it->first, qty);
                if ((it->second.qty -= qty) == 0) live.erase(it);
            }
            if (step % 50 != 0) continue;

            f.flush();
            const auto& book = f.book();
            ASSERT_TRUE(book.validate()) << "seed " << seed << " step " << step;
            ASSERT_EQ(book.order_count(), live.size());
            std::map<std::pair<int, Price>, Qty> levels;
            for (const auto& [id, o] : live) {
                ASSERT_EQ(book.order(id), (OrderInfo{o.side, o.price, o.qty}));
                levels[{static_cast<int>(o.side), o.price}] += o.qty;
            }
            std::size_t n = 0;
            for (Side side : {Side::Buy, Side::Sell})
                book.for_each_level(side, [&](Price p, Qty q) {
                    ++n;
                    EXPECT_EQ((levels[{static_cast<int>(side), p}]), q);
                });
            ASSERT_EQ(n, levels.size());
        }
        EXPECT_EQ(f.bb.stats().skipped(), 0u);
        EXPECT_EQ(f.bb.stats().unknown_ref, 0u);
    }
}

TEST(ItchBook, RejectsUnusableConfig) {
    auto build = [](BookConfig c) { BookBuilder b(c, "AAPL"); };
    BookConfig ok;
    EXPECT_NO_THROW(build(ok));
    BookConfig c = ok;
    c.tick = 0;
    EXPECT_THROW(build(c), std::invalid_argument);
    c = ok;
    c.max_levels = 1;
    EXPECT_THROW(build(c), std::invalid_argument);
    c = ok;
    c.max_orders = 0;
    EXPECT_THROW(build(c), std::invalid_argument);
    c = ok;
    c.window = 0;
    EXPECT_THROW(build(c), std::invalid_argument);
}
