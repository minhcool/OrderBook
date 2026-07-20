#include "orderbook/orderbook.h"

#include <algorithm>
#include <mutex>

double SubmitResult::averagePrice() const {
    if (filledQuantity == 0) {
        return 0.0;
    }

    return static_cast<double>(notional) / static_cast<double>(filledQuantity);
}

SubmitResult orderbook::submit(order incoming, Type type) {
    std::lock_guard<std::mutex> lock(bookMutex);
    return submitLocked(incoming, type);
}

SubmitResult orderbook::replace(order replacement, Type type) {
    std::lock_guard<std::mutex> lock(bookMutex);
    return replaceLocked(replacement, type);
}

bool orderbook::add(order incoming, Type type) {
    return submit(incoming, type).accepted;
}

SubmitResult orderbook::buy(TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return submit(order(orderId, traderId, price, quantity, Side::Buy), Type::Regular);
}

SubmitResult orderbook::sell(TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return submit(order(orderId, traderId, price, quantity, Side::Sell), Type::Regular);
}

SubmitResult orderbook::marketBuy(TraderId traderId, OrderId orderId, Qty quantity) {
    return submit(order(orderId, traderId, 0, quantity, Side::Buy), Type::Market);
}

SubmitResult orderbook::marketSell(TraderId traderId, OrderId orderId, Qty quantity) {
    return submit(order(orderId, traderId, 0, quantity, Side::Sell), Type::Market);
}

SubmitResult orderbook::iocBuy(TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return submit(order(orderId, traderId, price, quantity, Side::Buy), Type::IoC);
}

SubmitResult orderbook::iocSell(TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return submit(order(orderId, traderId, price, quantity, Side::Sell), Type::IoC);
}

SubmitResult orderbook::fokBuy(TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return submit(order(orderId, traderId, price, quantity, Side::Buy), Type::FoK);
}

SubmitResult orderbook::fokSell(TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return submit(order(orderId, traderId, price, quantity, Side::Sell), Type::FoK);
}

SubmitResult orderbook::replaceBuy(TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return replace(order(orderId, traderId, price, quantity, Side::Buy), Type::Regular);
}

SubmitResult orderbook::replaceSell(TraderId traderId, OrderId orderId, Price price, Qty quantity) {
    return replace(order(orderId, traderId, price, quantity, Side::Sell), Type::Regular);
}

bool orderbook::cancel(OrderId id) {
    std::lock_guard<std::mutex> lock(bookMutex);
    return removeById(id);
}

bool orderbook::delBuy(const order& target) {
    if (target.getSide() != Side::Buy) {
        return false;
    }

    return cancel(target.getId());
}

bool orderbook::delSell(const order& target) {
    if (target.getSide() != Side::Sell) {
        return false;
    }

    return cancel(target.getId());
}

bool orderbook::changeOrder(const order& target, Qty newQuant, Price newPrice, Side side) {
    std::lock_guard<std::mutex> lock(bookMutex);

    SubmitResult result = replaceLocked(
        order(target.getId(), target.getTraderId(), newPrice, newQuant, side),
        Type::Regular);

    return result.accepted;
}

bool orderbook::empty() const {
    std::lock_guard<std::mutex> lock(bookMutex);
    return buys.empty() && sells.empty();
}

Qty orderbook::totalBuyQuantity() const {
    std::lock_guard<std::mutex> lock(bookMutex);
    return totalBuyQuantityLocked();
}

Qty orderbook::totalSellQuantity() const {
    std::lock_guard<std::mutex> lock(bookMutex);
    return totalSellQuantityLocked();
}

Qty orderbook::totalQuantityAtPrice(Side side, Price price) const {
    std::lock_guard<std::mutex> lock(bookMutex);
    return totalQuantityAtPriceLocked(side, price);
}

bool orderbook::hasBestBid() const {
    std::lock_guard<std::mutex> lock(bookMutex);
    return !buys.empty();
}

bool orderbook::hasBestAsk() const {
    std::lock_guard<std::mutex> lock(bookMutex);
    return !sells.empty();
}

Price orderbook::bestBid() const {
    std::lock_guard<std::mutex> lock(bookMutex);
    return buys.empty() ? 0 : buys.begin()->first;
}

Price orderbook::bestAsk() const {
    std::lock_guard<std::mutex> lock(bookMutex);
    return sells.empty() ? 0 : sells.begin()->first;
}

BookSnapshot orderbook::snapshot(std::size_t depth) const {
    std::lock_guard<std::mutex> lock(bookMutex);

    BookSnapshot result;

    for (const auto& [price, queue] : buys) {
        if (result.bids.size() >= depth) {
            break;
        }

        Qty total = 0;
        for (const order& resting : queue) {
            total += resting.getRemainingQuantity();
        }

        result.bids.push_back({price, total});
    }

    for (const auto& [price, queue] : sells) {
        if (result.asks.size() >= depth) {
            break;
        }

        Qty total = 0;
        for (const order& resting : queue) {
            total += resting.getRemainingQuantity();
        }

        result.asks.push_back({price, total});
    }

    return result;
}

void orderbook::print(std::ostream& os) const {
    std::lock_guard<std::mutex> lock(bookMutex);

    os << "SELLS\n";
    for (const auto& [price, queue] : sells) {
        os << "  " << price << " x " << totalQuantityAtPriceLocked(Side::Sell, price)
           << " (" << queue.size() << " orders)\n";
    }

    os << "BUYS\n";
    for (const auto& [price, queue] : buys) {
        os << "  " << price << " x " << totalQuantityAtPriceLocked(Side::Buy, price)
           << " (" << queue.size() << " orders)\n";
    }
}

bool orderbook::isValidSide(Side side) {
    return side == Side::Buy || side == Side::Sell;
}

Price orderbook::effectiveLimitPrice(const order& incoming, Type type) {
    if (type != Type::Market) {
        return incoming.getPrice();
    }

    return incoming.getSide() == Side::Buy ? Constants::MAXPRICE : 0;
}

SubmitResult orderbook::submitLocked(order incoming, Type type) {
    SubmitResult result;

    if (!validateIncoming(incoming, type, result)) {
        return result;
    }

    if (hasRestingOrder(incoming.getId())) {
        result.message = "order id is already resting on the book; use replace()";
        return result;
    }

    const Price limitPrice = effectiveLimitPrice(incoming, type);

    if (type == Type::FoK && !hasEnoughLiquidity(incoming, limitPrice)) {
        result.message = "fill-or-kill order could not be fully filled";
        return result;
    }

    result.accepted = true;
    match(incoming, limitPrice, result);

    if (!result.selfTradePrevented && type == Type::Regular && incoming.getRemainingQuantity() > 0) {
        incoming.setPrice(limitPrice);
        rest(incoming);
        result.restingQuantity = incoming.getRemainingQuantity();
    }

    if (result.selfTradePrevented && result.filledQuantity > 0) {
        result.message = "partially traded; remaining quantity canceled to prevent self-trade";
    } else if (result.selfTradePrevented) {
        result.accepted = false;
        result.message = "self-trade prevented";
    } else if (result.filledQuantity > 0) {
        result.message = "traded";
    } else if (result.restingQuantity > 0) {
        result.message = "resting";
    } else {
        result.message = "no liquidity";
    }

    return result;
}

SubmitResult orderbook::replaceLocked(order replacement, Type type) {
    SubmitResult result;

    if (!validateIncoming(replacement, type, result)) {
        return result;
    }

    if (!hasRestingOrder(replacement.getId())) {
        result.message = "order id is not resting on the book";
        return result;
    }

    removeById(replacement.getId());

    const Price limitPrice = effectiveLimitPrice(replacement, type);

    if (type == Type::FoK && !hasEnoughLiquidity(replacement, limitPrice)) {
        result.message = "fill-or-kill replacement could not be fully filled";
        return result;
    }

    result.accepted = true;
    match(replacement, limitPrice, result);

    if (!result.selfTradePrevented && type == Type::Regular && replacement.getRemainingQuantity() > 0) {
        replacement.setPrice(limitPrice);
        rest(replacement);
        result.restingQuantity = replacement.getRemainingQuantity();
    }

    if (result.selfTradePrevented && result.filledQuantity > 0) {
        result.message = "replacement partially traded; remaining quantity canceled to prevent self-trade";
    } else if (result.selfTradePrevented) {
        result.accepted = false;
        result.message = "replacement canceled to prevent self-trade";
    } else if (result.filledQuantity > 0) {
        result.message = "replaced and traded";
    } else if (result.restingQuantity > 0) {
        result.message = "replaced and resting";
    } else {
        result.message = "replaced; no liquidity";
    }

    return result;
}

bool orderbook::validateIncoming(const order& incoming, Type type, SubmitResult& result) const {
    if (!isValidSide(incoming.getSide())) {
        result.message = "invalid side";
        return false;
    }

    if (incoming.getQuantity() <= 0) {
        result.message = "quantity must be positive";
        return false;
    }

    if (type != Type::Market && incoming.getPrice() <= 0) {
        result.message = "price must be positive";
        return false;
    }

    return true;
}

bool orderbook::hasRestingOrder(OrderId id) const {
    const auto hasId = [id](const auto& book) {
        for (const auto& [price, queue] : book) {
            (void)price;
            const auto found = std::find_if(queue.begin(), queue.end(), [id](const order& resting) {
                return resting.getId() == id;
            });

            if (found != queue.end()) {
                return true;
            }
        }

        return false;
    };

    return hasId(buys) || hasId(sells);
}

bool orderbook::hasEnoughLiquidity(const order& incoming, Price limitPrice) const {
    Qty remaining = incoming.getRemainingQuantity();

    if (incoming.getSide() == Side::Buy) {
        for (const auto& [price, queue] : sells) {
            if (price > limitPrice) {
                break;
            }

            for (const order& resting : queue) {
                if (resting.getTraderId() == incoming.getTraderId()) {
                    return false;
                }

                remaining -= resting.getRemainingQuantity();
                if (remaining <= 0) {
                    return true;
                }
            }
        }
    } else {
        for (const auto& [price, queue] : buys) {
            if (price < limitPrice) {
                break;
            }

            for (const order& resting : queue) {
                if (resting.getTraderId() == incoming.getTraderId()) {
                    return false;
                }

                remaining -= resting.getRemainingQuantity();
                if (remaining <= 0) {
                    return true;
                }
            }
        }
    }

    return remaining <= 0;
}

Qty orderbook::totalBuyQuantityLocked() const {
    Qty total = 0;
    for (const auto& [price, queue] : buys) {
        (void)price;
        for (const order& resting : queue) {
            total += resting.getRemainingQuantity();
        }
    }

    return total;
}

Qty orderbook::totalSellQuantityLocked() const {
    Qty total = 0;
    for (const auto& [price, queue] : sells) {
        (void)price;
        for (const order& resting : queue) {
            total += resting.getRemainingQuantity();
        }
    }

    return total;
}

Qty orderbook::totalQuantityAtPriceLocked(Side side, Price price) const {
    Qty total = 0;

    if (side == Side::Buy) {
        const auto level = buys.find(price);
        if (level == buys.end()) {
            return 0;
        }

        for (const order& resting : level->second) {
            total += resting.getRemainingQuantity();
        }
    } else {
        const auto level = sells.find(price);
        if (level == sells.end()) {
            return 0;
        }

        for (const order& resting : level->second) {
            total += resting.getRemainingQuantity();
        }
    }

    return total;
}

void orderbook::match(order& incoming, Price limitPrice, SubmitResult& result) {
    if (incoming.getSide() == Side::Buy) {
        while (incoming.getRemainingQuantity() > 0 && !sells.empty()) {
            auto best = sells.begin();
            const Price tradePrice = best->first;
            if (tradePrice > limitPrice) {
                break;
            }

            auto& queue = best->second;
            order& resting = queue.front();
            if (resting.getTraderId() == incoming.getTraderId()) {
                result.selfTradePrevented = true;
                incoming.setRemainingQuantity(0);
                return;
            }

            const Qty tradeQty = std::min(incoming.getRemainingQuantity(), resting.getRemainingQuantity());

            incoming.setRemainingQuantity(incoming.getRemainingQuantity() - tradeQty);
            resting.setRemainingQuantity(resting.getRemainingQuantity() - tradeQty);

            result.filledQuantity += tradeQty;
            result.notional += tradeQty * tradePrice;
            result.trades.push_back({
                incoming.getId(),
                resting.getId(),
                incoming.getTraderId(),
                resting.getTraderId(),
                incoming.getSide(),
                tradePrice,
                tradeQty,
            });

            if (resting.isFilled()) {
                queue.pop_front();
            }

            if (queue.empty()) {
                sells.erase(best);
            }
        }

        return;
    }

    while (incoming.getRemainingQuantity() > 0 && !buys.empty()) {
        auto best = buys.begin();
        const Price tradePrice = best->first;
        if (tradePrice < limitPrice) {
            break;
        }

        auto& queue = best->second;
        order& resting = queue.front();
        if (resting.getTraderId() == incoming.getTraderId()) {
            result.selfTradePrevented = true;
            incoming.setRemainingQuantity(0);
            return;
        }

        const Qty tradeQty = std::min(incoming.getRemainingQuantity(), resting.getRemainingQuantity());

        incoming.setRemainingQuantity(incoming.getRemainingQuantity() - tradeQty);
        resting.setRemainingQuantity(resting.getRemainingQuantity() - tradeQty);

        result.filledQuantity += tradeQty;
        result.notional += tradeQty * tradePrice;
        result.trades.push_back({
            incoming.getId(),
            resting.getId(),
            incoming.getTraderId(),
            resting.getTraderId(),
            incoming.getSide(),
            tradePrice,
            tradeQty,
        });

        if (resting.isFilled()) {
            queue.pop_front();
        }

        if (queue.empty()) {
            buys.erase(best);
        }
    }
}

void orderbook::rest(order incoming) {
    if (incoming.getRemainingQuantity() <= 0) {
        return;
    }

    if (incoming.getSide() == Side::Buy) {
        buys[incoming.getPrice()].push_back(incoming);
    } else {
        sells[incoming.getPrice()].push_back(incoming);
    }
}

bool orderbook::removeById(OrderId id) {
    for (auto level = buys.begin(); level != buys.end(); ++level) {
        auto& queue = level->second;
        const auto found = std::find_if(queue.begin(), queue.end(), [id](const order& resting) {
            return resting.getId() == id;
        });

        if (found != queue.end()) {
            queue.erase(found);
            if (queue.empty()) {
                buys.erase(level);
            }
            return true;
        }
    }

    for (auto level = sells.begin(); level != sells.end(); ++level) {
        auto& queue = level->second;
        const auto found = std::find_if(queue.begin(), queue.end(), [id](const order& resting) {
            return resting.getId() == id;
        });

        if (found != queue.end()) {
            queue.erase(found);
            if (queue.empty()) {
                sells.erase(level);
            }
            return true;
        }
    }

    return false;
}
