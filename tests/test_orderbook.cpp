#include "orderbook/exchange.h"
#include "orderbook/orderbook.h"

#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

order makeOrder(OrderId id, TraderId traderId, Price price, Qty quantity, Side side) {
    return order(id, traderId, price, quantity, side);
}

void test_regular_matching_and_partial_rest() {
    orderbook book;

    SubmitResult sell1 = book.submit(makeOrder(1, 10, 100, 10, Side::Sell));
    assert(sell1.accepted);
    assert(sell1.filledQuantity == 0);
    assert(sell1.restingQuantity == 10);
    assert(book.bestAsk() == 100);

    SubmitResult buy1 = book.submit(makeOrder(2, 20, 99, 4, Side::Buy));
    assert(buy1.accepted);
    assert(buy1.filledQuantity == 0);
    assert(buy1.restingQuantity == 4);
    assert(book.bestBid() == 99);

    SubmitResult buy2 = book.submit(makeOrder(3, 30, 100, 7, Side::Buy));
    assert(buy2.accepted);
    assert(buy2.filledQuantity == 7);
    assert(buy2.restingQuantity == 0);
    assert(buy2.trades.size() == 1);
    assert(buy2.trades[0].makerId == 1);
    assert(buy2.trades[0].makerTraderId == 10);
    assert(buy2.trades[0].takerTraderId == 30);
    assert(buy2.trades[0].price == 100);
    assert(book.totalQuantityAtPrice(Side::Sell, 100) == 3);

    SubmitResult buy3 = book.submit(makeOrder(4, 40, 105, 5, Side::Buy));
    assert(buy3.accepted);
    assert(buy3.filledQuantity == 3);
    assert(buy3.restingQuantity == 2);
    assert(book.totalQuantityAtPrice(Side::Buy, 105) == 2);
    assert(!book.hasBestAsk());

    SubmitResult iocSell = book.submit(makeOrder(5, 50, 105, 5, Side::Sell), Type::IoC);
    assert(iocSell.accepted);
    assert(iocSell.filledQuantity == 2);
    assert(iocSell.restingQuantity == 0);
    assert(book.totalQuantityAtPrice(Side::Sell, 105) == 0);

    SubmitResult restingSell = book.submit(makeOrder(6, 60, 101, 3, Side::Sell));
    assert(restingSell.accepted);
    assert(book.cancel(6));
    assert(!book.hasBestAsk());

    SubmitResult fokFail = book.submit(makeOrder(7, 70, 99, 10, Side::Sell), Type::FoK);
    assert(!fokFail.accepted);
    assert(book.totalBuyQuantity() == 4);

    assert(book.changeOrder(makeOrder(2, 20, 99, 4, Side::Buy), 6, 98, Side::Buy));
    assert(book.bestBid() == 98);
    assert(book.totalQuantityAtPrice(Side::Buy, 98) == 6);
}

void test_fifo_at_same_price() {
    orderbook book;

    assert(book.submit(makeOrder(1, 10, 100, 3, Side::Sell)).accepted);
    assert(book.submit(makeOrder(2, 20, 100, 4, Side::Sell)).accepted);

    SubmitResult buy = book.submit(makeOrder(3, 30, 100, 5, Side::Buy));

    assert(buy.accepted);
    assert(buy.filledQuantity == 5);
    assert(buy.trades.size() == 2);
    assert(buy.trades[0].makerId == 1);
    assert(buy.trades[0].quantity == 3);
    assert(buy.trades[1].makerId == 2);
    assert(buy.trades[1].quantity == 2);
    assert(book.totalQuantityAtPrice(Side::Sell, 100) == 2);
}

void test_multi_level_matching() {
    orderbook book;

    assert(book.submit(makeOrder(1, 10, 101, 2, Side::Sell)).accepted);
    assert(book.submit(makeOrder(2, 20, 102, 3, Side::Sell)).accepted);
    assert(book.submit(makeOrder(3, 30, 103, 4, Side::Sell)).accepted);

    SubmitResult buy = book.submit(makeOrder(4, 40, 103, 6, Side::Buy));

    assert(buy.accepted);
    assert(buy.filledQuantity == 6);
    assert(buy.restingQuantity == 0);
    assert(buy.trades.size() == 3);
    assert(buy.trades[0].price == 101);
    assert(buy.trades[0].quantity == 2);
    assert(buy.trades[1].price == 102);
    assert(buy.trades[1].quantity == 3);
    assert(buy.trades[2].price == 103);
    assert(buy.trades[2].quantity == 1);
    assert(buy.notional == 611);
    assert(book.bestAsk() == 103);
    assert(book.totalQuantityAtPrice(Side::Sell, 103) == 3);
}

void test_market_orders() {
    orderbook book;

    assert(book.submit(makeOrder(1, 10, 100, 10, Side::Sell)).accepted);
    assert(book.submit(makeOrder(2, 20, 101, 4, Side::Sell)).accepted);

    SubmitResult marketBuy = book.submit(makeOrder(3, 30, 0, 12, Side::Buy), Type::Market);

    assert(marketBuy.accepted);
    assert(marketBuy.filledQuantity == 12);
    assert(marketBuy.restingQuantity == 0);
    assert(marketBuy.trades.size() == 2);
    assert(marketBuy.trades[0].price == 100);
    assert(marketBuy.trades[0].quantity == 10);
    assert(marketBuy.trades[1].price == 101);
    assert(marketBuy.trades[1].quantity == 2);
    assert(!book.hasBestBid());
    assert(book.bestAsk() == 101);
    assert(book.totalQuantityAtPrice(Side::Sell, 101) == 2);

    orderbook sellBook;
    assert(sellBook.submit(makeOrder(4, 40, 99, 3, Side::Buy)).accepted);
    assert(sellBook.submit(makeOrder(5, 50, 98, 4, Side::Buy)).accepted);

    SubmitResult marketSell = sellBook.submit(makeOrder(6, 60, 0, 5, Side::Sell), Type::Market);

    assert(marketSell.accepted);
    assert(marketSell.filledQuantity == 5);
    assert(marketSell.restingQuantity == 0);
    assert(marketSell.trades.size() == 2);
    assert(marketSell.trades[0].price == 99);
    assert(marketSell.trades[0].quantity == 3);
    assert(marketSell.trades[1].price == 98);
    assert(marketSell.trades[1].quantity == 2);
    assert(!sellBook.hasBestAsk());
    assert(sellBook.bestBid() == 98);
    assert(sellBook.totalQuantityAtPrice(Side::Buy, 98) == 2);
}

void test_ioc_orders() {
    orderbook noFillBook;

    assert(noFillBook.submit(makeOrder(1, 10, 110, 5, Side::Sell)).accepted);

    SubmitResult noFill = noFillBook.submit(makeOrder(2, 20, 100, 3, Side::Buy), Type::IoC);

    assert(noFill.accepted);
    assert(noFill.filledQuantity == 0);
    assert(noFill.restingQuantity == 0);
    assert(noFillBook.totalBuyQuantity() == 0);
    assert(noFillBook.totalQuantityAtPrice(Side::Sell, 110) == 5);

    orderbook partialBook;

    assert(partialBook.submit(makeOrder(3, 30, 100, 2, Side::Sell)).accepted);

    SubmitResult partial = partialBook.submit(makeOrder(4, 40, 101, 5, Side::Buy), Type::IoC);

    assert(partial.accepted);
    assert(partial.filledQuantity == 2);
    assert(partial.restingQuantity == 0);
    assert(partialBook.empty());
}

void test_fok_orders() {
    orderbook successBook;

    assert(successBook.submit(makeOrder(1, 10, 100, 2, Side::Sell)).accepted);
    assert(successBook.submit(makeOrder(2, 20, 101, 3, Side::Sell)).accepted);

    SubmitResult success = successBook.submit(makeOrder(3, 30, 101, 5, Side::Buy), Type::FoK);

    assert(success.accepted);
    assert(success.filledQuantity == 5);
    assert(success.restingQuantity == 0);
    assert(successBook.empty());

    orderbook failBook;

    assert(failBook.submit(makeOrder(4, 40, 100, 2, Side::Sell)).accepted);
    assert(failBook.submit(makeOrder(5, 50, 101, 3, Side::Sell)).accepted);

    SubmitResult fail = failBook.submit(makeOrder(6, 60, 101, 6, Side::Buy), Type::FoK);

    assert(!fail.accepted);
    assert(fail.filledQuantity == 0);
    assert(failBook.totalSellQuantity() == 5);
    assert(failBook.bestAsk() == 100);
}

void test_duplicate_ids_require_explicit_replace() {
    orderbook book;

    assert(book.submit(makeOrder(1, 10, 100, 5, Side::Buy)).accepted);

    SubmitResult duplicate = book.submit(makeOrder(1, 20, 101, 3, Side::Sell));

    assert(!duplicate.accepted);
    assert(book.totalBuyQuantity() == 5);
    assert(book.bestBid() == 100);

    SubmitResult replacement = book.replace(makeOrder(1, 10, 102, 7, Side::Buy));

    assert(replacement.accepted);
    assert(replacement.filledQuantity == 0);
    assert(replacement.restingQuantity == 7);
    assert(book.bestBid() == 102);
    assert(book.totalQuantityAtPrice(Side::Buy, 100) == 0);
    assert(book.totalQuantityAtPrice(Side::Buy, 102) == 7);

    assert(!book.cancel(42));
    assert(book.cancel(1));
    assert(book.empty());
}

void test_replace_and_cancel_require_owner() {
    orderbook book;

    assert(book.buy(10, 1, 100, 5).accepted);

    SubmitResult wrongTraderReplace = book.replaceBuy(20, 1, 101, 5);
    assert(!wrongTraderReplace.accepted);
    assert(book.totalQuantityAtPrice(Side::Buy, 100) == 5);
    assert(book.totalQuantityAtPrice(Side::Buy, 101) == 0);

    assert(!book.cancelForTrader(20, 1));
    assert(book.totalQuantityAtPrice(Side::Buy, 100) == 5);

    assert(book.cancelForTrader(10, 1));
    assert(book.empty());
}

void test_named_order_entry_api_methods() {
    orderbook book;

    SubmitResult restingSell = book.sell(10, 1, 100, 5);
    assert(restingSell.accepted);
    assert(restingSell.restingQuantity == 5);

    SubmitResult regularBuy = book.buy(20, 2, 100, 2);
    assert(regularBuy.accepted);
    assert(regularBuy.filledQuantity == 2);
    assert(regularBuy.trades.size() == 1);
    assert(regularBuy.trades[0].price == 100);
    assert(book.totalQuantityAtPrice(Side::Sell, 100) == 3);

    SubmitResult marketBuy = book.marketBuy(30, 3, 3);
    assert(marketBuy.accepted);
    assert(marketBuy.filledQuantity == 3);
    assert(marketBuy.restingQuantity == 0);
    assert(book.empty());

    SubmitResult restingBuy = book.buy(40, 4, 99, 4);
    assert(restingBuy.accepted);
    assert(restingBuy.restingQuantity == 4);

    SubmitResult iocSell = book.iocSell(50, 5, 99, 2);
    assert(iocSell.accepted);
    assert(iocSell.filledQuantity == 2);
    assert(iocSell.restingQuantity == 0);
    assert(book.totalQuantityAtPrice(Side::Buy, 99) == 2);

    SubmitResult fokSellFail = book.fokSell(60, 6, 99, 3);
    assert(!fokSellFail.accepted);
    assert(book.totalQuantityAtPrice(Side::Buy, 99) == 2);

    SubmitResult replaceBuy = book.replaceBuy(40, 4, 98, 6);
    assert(replaceBuy.accepted);
    assert(replaceBuy.restingQuantity == 6);
    assert(book.totalQuantityAtPrice(Side::Buy, 99) == 0);
    assert(book.totalQuantityAtPrice(Side::Buy, 98) == 6);

    SubmitResult marketSell = book.marketSell(70, 7, 6);
    assert(marketSell.accepted);
    assert(marketSell.filledQuantity == 6);
    assert(book.empty());

    SubmitResult restingSellForBuy = book.sell(80, 8, 101, 4);
    assert(restingSellForBuy.accepted);

    SubmitResult iocBuy = book.iocBuy(90, 9, 100, 4);
    assert(iocBuy.accepted);
    assert(iocBuy.filledQuantity == 0);
    assert(iocBuy.restingQuantity == 0);
    assert(book.totalQuantityAtPrice(Side::Sell, 101) == 4);

    SubmitResult fokBuy = book.fokBuy(90, 10, 101, 4);
    assert(fokBuy.accepted);
    assert(fokBuy.filledQuantity == 4);
    assert(book.empty());

    SubmitResult sellToReplace = book.sell(100, 11, 105, 3);
    assert(sellToReplace.accepted);

    SubmitResult replaceSell = book.replaceSell(100, 11, 106, 5);
    assert(replaceSell.accepted);
    assert(replaceSell.restingQuantity == 5);
    assert(book.totalQuantityAtPrice(Side::Sell, 105) == 0);
    assert(book.totalQuantityAtPrice(Side::Sell, 106) == 5);
}

void test_book_snapshot() {
    orderbook book;

    assert(book.buy(10, 1, 100, 2).accepted);
    assert(book.buy(20, 2, 99, 3).accepted);
    assert(book.buy(30, 3, 99, 4).accepted);
    assert(book.sell(40, 4, 105, 5).accepted);
    assert(book.sell(50, 5, 106, 6).accepted);

    BookSnapshot full = book.snapshot();
    assert(full.bids.size() == 2);
    assert(full.asks.size() == 2);
    assert(full.bids[0].price == 100);
    assert(full.bids[0].quantity == 2);
    assert(full.bids[1].price == 99);
    assert(full.bids[1].quantity == 7);
    assert(full.asks[0].price == 105);
    assert(full.asks[0].quantity == 5);
    assert(full.asks[1].price == 106);
    assert(full.asks[1].quantity == 6);

    BookSnapshot topOnly = book.snapshot(1);
    assert(topOnly.bids.size() == 1);
    assert(topOnly.asks.size() == 1);
    assert(topOnly.bids[0].price == 100);
    assert(topOnly.asks[0].price == 105);
}

void test_exchange_routes_by_symbol() {
    Exchange exchange;

    exchange.ensureBook("ETH-USD");
    std::vector<std::string> initialSymbols = exchange.symbols();
    assert(initialSymbols.size() == 1);
    assert(initialSymbols[0] == "ETH-USD");
    assert(exchange.snapshot("ETH-USD").bids.empty());
    assert(exchange.snapshot("ETH-USD").asks.empty());

    assert(exchange.sell("BTC-USD", 10, 1, 100, 5).accepted);
    assert(exchange.buy("ETH-USD", 20, 2, 50, 7).accepted);

    BookSnapshot btc = exchange.snapshot("BTC-USD");
    BookSnapshot eth = exchange.snapshot("ETH-USD");

    assert(btc.asks.size() == 1);
    assert(btc.asks[0].price == 100);
    assert(btc.asks[0].quantity == 5);
    assert(btc.bids.empty());

    assert(eth.bids.size() == 1);
    assert(eth.bids[0].price == 50);
    assert(eth.bids[0].quantity == 7);
    assert(eth.asks.empty());

    assert(exchange.cancel("BTC-USD", 1));
    assert(exchange.snapshot("BTC-USD").asks.empty());

    std::vector<std::string> symbols = exchange.symbols();
    assert(symbols.size() == 2);
    assert(symbols[0] == "BTC-USD");
    assert(symbols[1] == "ETH-USD");
}

void test_modify_order_that_crosses_spread() {
    orderbook book;

    assert(book.submit(makeOrder(1, 10, 100, 5, Side::Sell)).accepted);
    assert(book.submit(makeOrder(2, 20, 95, 5, Side::Buy)).accepted);

    assert(book.changeOrder(makeOrder(2, 20, 95, 5, Side::Buy), 5, 100, Side::Buy));

    assert(book.empty());
}

void test_self_trade_prevention_rejects_incoming_order() {
    orderbook book;

    assert(book.submit(makeOrder(1, 10, 100, 5, Side::Sell)).accepted);

    SubmitResult buy = book.submit(makeOrder(2, 10, 100, 5, Side::Buy));

    assert(!buy.accepted);
    assert(buy.selfTradePrevented);
    assert(buy.filledQuantity == 0);
    assert(book.totalQuantityAtPrice(Side::Sell, 100) == 5);
    assert(book.totalBuyQuantity() == 0);
}

void test_self_trade_prevention_after_partial_external_fill() {
    orderbook book;

    assert(book.submit(makeOrder(1, 10, 100, 2, Side::Sell)).accepted);
    assert(book.submit(makeOrder(2, 20, 100, 3, Side::Sell)).accepted);

    SubmitResult buy = book.submit(makeOrder(3, 20, 100, 5, Side::Buy));

    assert(buy.accepted);
    assert(buy.selfTradePrevented);
    assert(buy.filledQuantity == 2);
    assert(buy.restingQuantity == 0);
    assert(buy.trades.size() == 1);
    assert(buy.trades[0].makerId == 1);
    assert(book.totalQuantityAtPrice(Side::Sell, 100) == 3);
    assert(book.totalBuyQuantity() == 0);
}

void test_fok_does_not_count_same_trader_liquidity() {
    orderbook book;

    assert(book.submit(makeOrder(1, 10, 100, 2, Side::Sell)).accepted);
    assert(book.submit(makeOrder(2, 20, 100, 3, Side::Sell)).accepted);

    SubmitResult buy = book.submit(makeOrder(3, 20, 100, 5, Side::Buy), Type::FoK);

    assert(!buy.accepted);
    assert(!buy.selfTradePrevented);
    assert(buy.filledQuantity == 0);
    assert(book.totalQuantityAtPrice(Side::Sell, 100) == 5);
}

void test_coarse_lock_allows_concurrent_submits() {
    orderbook book;
    std::vector<std::thread> threads;

    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&book, i]() {
            for (int j = 0; j < 50; ++j) {
                const OrderId id = 1000 + i * 50 + j;
                const TraderId traderId = 100 + i;
                SubmitResult result = book.submit(makeOrder(id, traderId, 90 + i, 1, Side::Buy));
                assert(result.accepted);
            }
        });
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    assert(book.totalBuyQuantity() == 200);
}

int main() {
    test_regular_matching_and_partial_rest();
    test_fifo_at_same_price();
    test_multi_level_matching();
    test_market_orders();
    test_ioc_orders();
    test_fok_orders();
    test_duplicate_ids_require_explicit_replace();
    test_replace_and_cancel_require_owner();
    test_named_order_entry_api_methods();
    test_book_snapshot();
    test_exchange_routes_by_symbol();
    test_modify_order_that_crosses_spread();
    test_self_trade_prevention_rejects_incoming_order();
    test_self_trade_prevention_after_partial_external_fill();
    test_fok_does_not_count_same_trader_liquidity();
    test_coarse_lock_allows_concurrent_submits();

    std::cout << "All orderbook tests passed.\n";
}
