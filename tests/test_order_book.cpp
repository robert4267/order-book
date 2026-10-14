#include <gtest/gtest.h>

#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"
#include "test_util.hpp"

using namespace lob;

namespace {

// Every behaviour test runs against both books, so the reference model is
// held to the same rules as the fast one.
template <class B>
class BookTest : public ::testing::Test {
protected:
    B book{100, 200, 64};
    std::vector<Trade>& trades() { return book.listener().trades; }
};

using Books = ::testing::Types<OrderBook<TradeLog>, MapOrderBook<TradeLog>>;
TYPED_TEST_SUITE(BookTest, Books);

TYPED_TEST(BookTest, EmptyBook) {
    auto& b = this->book;
    EXPECT_FALSE(b.best_bid());
    EXPECT_FALSE(b.best_ask());
    EXPECT_EQ(b.order_count(), 0u);
}

TYPED_TEST(BookTest, RestsWhenNothingCrosses) {
    auto& b = this->book;
    EXPECT_EQ(b.add(1, Side::Buy, 150, 10), Status::Ok);
    EXPECT_EQ(b.add(2, Side::Sell, 151, 5), Status::Ok);
    EXPECT_EQ(b.best_bid(), 150);
    EXPECT_EQ(b.best_ask(), 151);
    EXPECT_EQ(b.depth_at(Side::Buy, 150), 10u);
    EXPECT_EQ(b.depth_at(Side::Sell, 151), 5u);
    EXPECT_TRUE(this->trades().empty());
}

TYPED_TEST(BookTest, FullFillAtMakerPrice) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 150, 10), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 155, 10), Status::Ok);
    ASSERT_EQ(this->trades().size(), 1u);
    EXPECT_EQ(this->trades()[0], (Trade{2, 1, 150, 10}));
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_FALSE(b.best_ask());
    EXPECT_FALSE(b.best_bid());
}

TYPED_TEST(BookTest, PartialFillRestsTheRemainder) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 150, 4), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 150, 10), Status::Ok);
    EXPECT_EQ(this->trades().size(), 1u);
    EXPECT_FALSE(b.best_ask());
    EXPECT_EQ(b.best_bid(), 150);
    EXPECT_EQ(b.order_qty(2), 6u);
}

TYPED_TEST(BookTest, TimePriorityWithinALevel) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, 5), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 150, 5), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Buy, 150, 5), Status::Ok);
    ASSERT_EQ(b.add(4, Side::Sell, 150, 12), Status::Ok);
    std::vector<Trade> expected{{4, 1, 150, 5}, {4, 2, 150, 5}, {4, 3, 150, 2}};
    EXPECT_EQ(this->trades(), expected);
    EXPECT_EQ(b.order_qty(3), 3u);
}

TYPED_TEST(BookTest, SweepsLevelsBestPriceFirst) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 153, 5), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 151, 5), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Sell, 152, 5), Status::Ok);
    ASSERT_EQ(b.add(4, Side::Buy, 152, 20), Status::Ok);
    std::vector<Trade> expected{{4, 2, 151, 5}, {4, 3, 152, 5}};
    EXPECT_EQ(this->trades(), expected);
    EXPECT_EQ(b.best_bid(), 152);
    EXPECT_EQ(b.order_qty(4), 10u);
    EXPECT_EQ(b.best_ask(), 153);
}

TYPED_TEST(BookTest, MarketOrderNeverRests) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 140, 5), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 120, 5), Status::Ok);
    EXPECT_EQ(b.market(9, Side::Sell, 100), Status::Ok);
    EXPECT_EQ(this->trades().size(), 2u);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_FALSE(b.best_ask());
    EXPECT_FALSE(b.order_qty(9));
}

TYPED_TEST(BookTest, MarketOrderOnEmptyBook) {
    EXPECT_EQ(this->book.market(1, Side::Buy, 10), Status::Ok);
    EXPECT_TRUE(this->trades().empty());
}

TYPED_TEST(BookTest, CancelFromMiddleOfQueue) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 160, 1), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 160, 2), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Sell, 160, 3), Status::Ok);
    EXPECT_EQ(b.cancel(2), Status::Ok);
    EXPECT_EQ(b.depth_at(Side::Sell, 160), 4u);
    ASSERT_EQ(b.add(4, Side::Buy, 160, 4), Status::Ok);
    std::vector<Trade> expected{{4, 1, 160, 1}, {4, 3, 160, 3}};
    EXPECT_EQ(this->trades(), expected);
}

TYPED_TEST(BookTest, CancelLastOrderMovesBestPrice) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, 1), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 130, 1), Status::Ok);
    ASSERT_EQ(b.cancel(1), Status::Ok);
    EXPECT_EQ(b.best_bid(), 130);
    ASSERT_EQ(b.cancel(2), Status::Ok);
    EXPECT_FALSE(b.best_bid());
}

TYPED_TEST(BookTest, CancelUnknownOrFilled) {
    auto& b = this->book;
    EXPECT_EQ(b.cancel(42), Status::UnknownId);
    ASSERT_EQ(b.add(1, Side::Sell, 150, 5), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 150, 5), Status::Ok);
    EXPECT_EQ(b.cancel(1), Status::UnknownId);
    EXPECT_EQ(b.cancel(2), Status::UnknownId);
}

TYPED_TEST(BookTest, ModifyDownKeepsPriority) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, 10), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 150, 10), Status::Ok);
    EXPECT_EQ(b.modify(1, 150, 3), Status::Ok);
    EXPECT_EQ(b.depth_at(Side::Buy, 150), 13u);
    ASSERT_EQ(b.add(3, Side::Sell, 150, 3), Status::Ok);
    ASSERT_EQ(this->trades().size(), 1u);
    EXPECT_EQ(this->trades()[0].maker, 1u);
}

TYPED_TEST(BookTest, ModifyUpLosesPriority) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, 10), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 150, 10), Status::Ok);
    ASSERT_EQ(b.modify(1, 150, 11), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Sell, 150, 1), Status::Ok);
    ASSERT_EQ(this->trades().size(), 1u);
    EXPECT_EQ(this->trades()[0].maker, 2u);
}

TYPED_TEST(BookTest, ModifyPriceCanCross) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 155, 5), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 150, 8), Status::Ok);
    EXPECT_EQ(b.modify(2, 155, 8), Status::Ok);
    ASSERT_EQ(this->trades().size(), 1u);
    EXPECT_EQ(this->trades()[0], (Trade{2, 1, 155, 5}));
    EXPECT_EQ(b.best_bid(), 155);
    EXPECT_EQ(b.order_qty(2), 3u);
}

TYPED_TEST(BookTest, RejectsBadInput) {
    auto& b = this->book;
    EXPECT_EQ(b.add(1, Side::Buy, 99, 1), Status::BadPrice);
    EXPECT_EQ(b.add(1, Side::Buy, 201, 1), Status::BadPrice);
    EXPECT_EQ(b.add(1, Side::Buy, 150, 0), Status::BadQty);
    EXPECT_EQ(b.market(1, Side::Buy, 0), Status::BadQty);
    EXPECT_EQ(b.add(1, Side::Buy, 150, 1), Status::Ok);
    EXPECT_EQ(b.add(1, Side::Sell, 180, 1), Status::DuplicateId);
    EXPECT_EQ(b.modify(1, 150, 0), Status::BadQty);
    EXPECT_EQ(b.modify(1, 500, 1), Status::BadPrice);
    EXPECT_EQ(b.modify(7, 150, 1), Status::UnknownId);
    EXPECT_EQ(b.order_count(), 1u);
}

TYPED_TEST(BookTest, IdCanBeReusedOnceGone) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, 1), Status::Ok);
    ASSERT_EQ(b.cancel(1), Status::Ok);
    EXPECT_EQ(b.add(1, Side::Sell, 160, 1), Status::Ok);
}

TYPED_TEST(BookTest, EdgesOfPriceRange) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 100, 1), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 200, 1), Status::Ok);
    EXPECT_EQ(b.best_bid(), 100);
    EXPECT_EQ(b.best_ask(), 200);
    ASSERT_EQ(b.market(3, Side::Sell, 1), Status::Ok);
    ASSERT_EQ(b.market(4, Side::Buy, 1), Status::Ok);
    EXPECT_EQ(this->trades().size(), 2u);
    EXPECT_EQ(b.order_count(), 0u);
}

TYPED_TEST(BookTest, BookFull) {
    TypeParam b(0, 10, 2);
    EXPECT_EQ(b.add(1, Side::Buy, 1, 1), Status::Ok);
    EXPECT_EQ(b.add(2, Side::Buy, 2, 1), Status::Ok);
    EXPECT_EQ(b.add(3, Side::Buy, 3, 1), Status::BookFull);
    ASSERT_EQ(b.cancel(1), Status::Ok);
    EXPECT_EQ(b.add(3, Side::Buy, 3, 1), Status::Ok);
}

TYPED_TEST(BookTest, IocFillsWhatItCanAndNeverRests) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 150, 3), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 152, 3), Status::Ok);
    EXPECT_EQ(b.add(3, Side::Buy, 151, 10, Tif::Ioc), Status::Ok);
    std::vector<Trade> expected{{3, 1, 150, 3}};
    EXPECT_EQ(this->trades(), expected);
    EXPECT_FALSE(b.order_qty(3));
    EXPECT_FALSE(b.best_bid());
    EXPECT_EQ(b.best_ask(), 152);
}

TYPED_TEST(BookTest, IocOnEmptyBookDoesNothing) {
    auto& b = this->book;
    EXPECT_EQ(b.add(1, Side::Buy, 150, 5, Tif::Ioc), Status::Ok);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_TRUE(this->trades().empty());
}

TYPED_TEST(BookTest, FokFillsAcrossLevels) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, 4), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 149, 4), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Buy, 140, 100), Status::Ok);
    EXPECT_EQ(b.add(4, Side::Sell, 149, 8, Tif::Fok), Status::Ok);
    std::vector<Trade> expected{{4, 1, 150, 4}, {4, 2, 149, 4}};
    EXPECT_EQ(this->trades(), expected);
    EXPECT_EQ(b.best_bid(), 140);
}

TYPED_TEST(BookTest, FokKilledLeavesBookUntouched) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, 4), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 149, 4), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Buy, 140, 100), Status::Ok);  // enough qty, but beyond the limit
    EXPECT_EQ(b.add(4, Side::Sell, 149, 9, Tif::Fok), Status::Killed);
    EXPECT_TRUE(this->trades().empty());
    EXPECT_EQ(b.order_qty(1), 4u);
    EXPECT_EQ(b.order_qty(2), 4u);
    EXPECT_FALSE(b.order_qty(4));
}

TYPED_TEST(BookTest, PostOnlyRestsOrIsRejected) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 150, 5), Status::Ok);
    EXPECT_EQ(b.add(2, Side::Buy, 150, 5, Tif::PostOnly), Status::WouldCross);
    EXPECT_EQ(b.add(3, Side::Buy, 149, 5, Tif::PostOnly), Status::Ok);
    EXPECT_TRUE(this->trades().empty());
    EXPECT_EQ(b.best_bid(), 149);
    EXPECT_FALSE(b.order_qty(2));
}

TYPED_TEST(BookTest, IocAndFokIgnoreBookFull) {
    // they never rest, so a full book is no reason to reject them
    TypeParam b(0, 10, 1);
    EXPECT_EQ(b.add(1, Side::Sell, 5, 3), Status::Ok);
    EXPECT_EQ(b.add(2, Side::Buy, 5, 1, Tif::Ioc), Status::Ok);
    EXPECT_EQ(b.add(3, Side::Buy, 5, 2, Tif::Fok), Status::Ok);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_EQ(b.add(4, Side::Buy, 4, 1, Tif::PostOnly), Status::Ok);
    EXPECT_EQ(b.add(5, Side::Buy, 3, 1, Tif::PostOnly), Status::BookFull);
}

TEST(OrderBook, LevelIterationIsBestFirst) {
    OrderBook<> b(0, 1000, 16);
    for (Price p : {500, 10, 900, 499})
        ASSERT_EQ(b.add(static_cast<OrderId>(p), Side::Buy, p, 1), Status::Ok);
    for (Price p : {901, 999, 950})
        ASSERT_EQ(b.add(static_cast<OrderId>(p), Side::Sell, p, 2), Status::Ok);

    std::vector<Price> bids, asks;
    b.for_each_level(Side::Buy, [&](Price p, Qty) { bids.push_back(p); });
    b.for_each_level(Side::Sell, [&](Price p, Qty) { asks.push_back(p); }, 2);
    EXPECT_EQ(bids, (std::vector<Price>{900, 500, 499, 10}));
    EXPECT_EQ(asks, (std::vector<Price>{901, 950}));
    EXPECT_TRUE(b.validate());
}

TEST(OrderBook, LevelsFarApart) {
    // bests that are tens of thousands of ticks apart go through the
    // summary layer of the bitmap
    OrderBook<> b(0, 1'000'000, 16);
    ASSERT_EQ(b.add(1, Side::Sell, 10, 1), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 900'000, 1), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Buy, 5, 1), Status::Ok);
    ASSERT_EQ(b.add(4, Side::Buy, 0, 1), Status::Ok);
    ASSERT_EQ(b.market(5, Side::Buy, 1), Status::Ok);
    EXPECT_EQ(b.best_ask(), 900'000);
    ASSERT_EQ(b.market(6, Side::Sell, 1), Status::Ok);
    EXPECT_EQ(b.best_bid(), 0);
    EXPECT_TRUE(b.validate());
}

TEST(OrderBook, NegativePrices) {
    OrderBook<TradeLog> b(-50, 50, 8);
    ASSERT_EQ(b.add(1, Side::Sell, -10, 3), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, -5, 5), Status::Ok);
    ASSERT_EQ(b.listener().trades.size(), 1u);
    EXPECT_EQ(b.listener().trades[0].price, -10);
    EXPECT_EQ(b.best_bid(), -5);
    EXPECT_TRUE(b.validate());
}

}  // namespace
