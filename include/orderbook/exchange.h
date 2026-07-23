#pragma once

#include "orderbook/orderbook.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Exchange {
public:
    void ensureBook(const std::string& symbol);

    SubmitResult buy(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult sell(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult marketBuy(const std::string& symbol, TraderId traderId, OrderId orderId, Qty quantity);
    SubmitResult marketSell(const std::string& symbol, TraderId traderId, OrderId orderId, Qty quantity);
    SubmitResult iocBuy(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult iocSell(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult fokBuy(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult fokSell(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult replaceBuy(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity);
    SubmitResult replaceSell(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity);

    bool cancel(const std::string& symbol, OrderId orderId);
    bool cancelForTrader(const std::string& symbol, TraderId traderId, OrderId orderId);
    BookSnapshot snapshot(const std::string& symbol, std::size_t depth = 10) const;
    std::vector<OpenOrder> openOrders(const std::string& symbol, TraderId traderId) const;
    std::vector<OpenOrder> openOrders(TraderId traderId) const;
    std::vector<std::string> symbols() const;

private:
    orderbook& getOrCreateBook(const std::string& symbol);
    const orderbook* findBook(const std::string& symbol) const;

    mutable std::mutex exchangeMutex;
    std::unordered_map<std::string, std::unique_ptr<orderbook>> books;
};
