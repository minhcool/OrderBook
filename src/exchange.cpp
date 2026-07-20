#include "orderbook/exchange.h"

#include <algorithm>
#include <utility>

SubmitResult Exchange::buy(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return getOrCreateBook(symbol).buy(traderId, orderId, price, quantity);
}

SubmitResult Exchange::sell(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return getOrCreateBook(symbol).sell(traderId, orderId, price, quantity);
}

SubmitResult Exchange::marketBuy(const std::string& symbol, TraderId traderId, OrderId orderId, Qty quantity) {
    return getOrCreateBook(symbol).marketBuy(traderId, orderId, quantity);
}

SubmitResult Exchange::marketSell(const std::string& symbol, TraderId traderId, OrderId orderId, Qty quantity) {
    return getOrCreateBook(symbol).marketSell(traderId, orderId, quantity);
}

SubmitResult Exchange::iocBuy(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return getOrCreateBook(symbol).iocBuy(traderId, orderId, price, quantity);
}

SubmitResult Exchange::iocSell(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return getOrCreateBook(symbol).iocSell(traderId, orderId, price, quantity);
}

SubmitResult Exchange::fokBuy(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return getOrCreateBook(symbol).fokBuy(traderId, orderId, price, quantity);
}

SubmitResult Exchange::fokSell(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return getOrCreateBook(symbol).fokSell(traderId, orderId, price, quantity);
}

SubmitResult Exchange::replaceBuy(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return getOrCreateBook(symbol).replaceBuy(traderId, orderId, price, quantity);
}

SubmitResult Exchange::replaceSell(const std::string& symbol, TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return getOrCreateBook(symbol).replaceSell(traderId, orderId, price, quantity);
}

bool Exchange::cancel(const std::string& symbol, OrderId orderId) {
    orderbook* book = nullptr;

    {
        std::lock_guard<std::mutex> lock(exchangeMutex);
        const auto found = books.find(symbol);
        if (found == books.end()) {
            return false;
        }

        book = found->second.get();
    }

    return book->cancel(orderId);
}

BookSnapshot Exchange::snapshot(const std::string& symbol, std::size_t depth) const {
    const orderbook* book = findBook(symbol);
    if (book == nullptr) {
        return {};
    }

    return book->snapshot(depth);
}

std::vector<std::string> Exchange::symbols() const {
    std::lock_guard<std::mutex> lock(exchangeMutex);

    std::vector<std::string> result;
    result.reserve(books.size());

    for (const auto& [symbol, book] : books) {
        (void)book;
        result.push_back(symbol);
    }

    std::sort(result.begin(), result.end());
    return result;
}

orderbook& Exchange::getOrCreateBook(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(exchangeMutex);

    auto found = books.find(symbol);
    if (found != books.end()) {
        return *found->second;
    }

    auto [inserted, wasInserted] = books.emplace(symbol, std::make_unique<orderbook>());
    (void)wasInserted;
    return *inserted->second;
}

const orderbook* Exchange::findBook(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(exchangeMutex);

    const auto found = books.find(symbol);
    if (found == books.end()) {
        return nullptr;
    }

    return found->second.get();
}
