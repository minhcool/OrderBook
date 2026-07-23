#pragma once

#include "orderbook/order.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

struct Trade {
    OrderId takerId = 0;
    OrderId makerId = 0;
    TraderId takerTraderId = 0;
    TraderId makerTraderId = 0;
    Side takerSide = Side::Buy;
    Price price = 0;
    Qty quantity = 0;
};

struct SubmitResult {
    OrderId orderId = 0;
    bool accepted = false;
    Qty filledQuantity = 0;
    Qty restingQuantity = 0;
    std::int64_t notional = 0;
    bool selfTradePrevented = false;
    std::string message;
    std::vector<Trade> trades;

    double averagePrice() const;
};

struct BookLevel {
    Price price = 0;
    Qty quantity = 0;
};

struct BookSnapshot {
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
};

struct OpenOrder {
    OrderId orderId = 0;
    TraderId traderId = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty quantity = 0;
    Qty remainingQuantity = 0;
};

class orderbook {
public:
    orderbook() = default;
    orderbook(const orderbook&) = delete;
    orderbook& operator=(const orderbook&) = delete;

    SubmitResult submit(order incoming, Type type = Type::Regular);
    SubmitResult replace(order replacement, Type type = Type::Regular);
    bool add(order incoming, Type type = Type::Regular);

    SubmitResult buy(TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult sell(TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult marketBuy(TraderId traderId, OrderId orderId, Qty quantity);
    SubmitResult marketSell(TraderId traderId, OrderId orderId, Qty quantity);
    SubmitResult iocBuy(TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult iocSell(TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult fokBuy(TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult fokSell(TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult replaceBuy(TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult replaceSell(TraderId traderId, OrderId orderId, Price price, Qty quantity);

    bool cancel(OrderId id);
    bool cancelForTrader(TraderId traderId, OrderId id);
    bool delBuy(const order& target);
    bool delSell(const order& target);
    bool changeOrder(const order& target, Qty newQuant, Price newPrice, Side side);

    bool empty() const;
    Qty totalBuyQuantity() const;
    Qty totalSellQuantity() const;
    Qty totalQuantityAtPrice(Side side, Price price) const;

    bool hasBestBid() const;
    bool hasBestAsk() const;
    Price bestBid() const;
    Price bestAsk() const;
    BookSnapshot snapshot(std::size_t depth = 10) const;
    std::vector<OpenOrder> openOrders(TraderId traderId) const;

    void print(std::ostream& os) const;

private:
    using BuyBook = std::map<Price, std::deque<order>, std::greater<Price>>;
    using SellBook = std::map<Price, std::deque<order>>;

    BuyBook buys;
    SellBook sells;
    mutable std::mutex bookMutex;

    static bool isValidSide(Side side);
    static Price effectiveLimitPrice(const order& incoming, Type type);

    SubmitResult submitLocked(order incoming, Type type);
    SubmitResult replaceLocked(order replacement, Type type);
    bool validateIncoming(const order& incoming, Type type, SubmitResult& result) const;
    bool hasRestingOrder(OrderId id) const;
    bool hasRestingOrderForTrader(OrderId id, TraderId traderId) const;
    bool hasEnoughLiquidity(const order& incoming, Price limitPrice) const;
    Qty totalBuyQuantityLocked() const;
    Qty totalSellQuantityLocked() const;
    Qty totalQuantityAtPriceLocked(Side side, Price price) const;
    void match(order& incoming, Price limitPrice, SubmitResult& result);
    void rest(order incoming);
    bool removeById(OrderId id);
    bool removeByIdForTrader(OrderId id, TraderId traderId);
};
