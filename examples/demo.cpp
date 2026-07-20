#include "orderbook/orderbook.h"

#include <iostream>

int main() {
    orderbook book;

    book.sell(10, 1, 100, 10);
    book.sell(20, 2, 101, 5);

    SubmitResult result = book.buy(30, 3, 101, 12);

    std::cout << "Filled " << result.filledQuantity << " at average price "
              << result.averagePrice() << "\n";

    for (const Trade& trade : result.trades) {
        std::cout << "Trade: taker " << trade.takerId
                  << " trader " << trade.takerTraderId
                  << " maker " << trade.makerId
                  << " trader " << trade.makerTraderId
                  << " price " << trade.price
                  << " quantity " << trade.quantity << "\n";
    }

    std::cout << "\nRemaining book:\n";
    book.print(std::cout);
}
