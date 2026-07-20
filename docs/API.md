# Orderbook API

This document describes the current C++ order-entry API.

Include:

```cpp
#include "orderbook/orderbook.h"
```

Create a book:

```cpp
orderbook book;
```

All public `orderbook` methods take a coarse mutex internally, so calling them from multiple threads is safe at the object level. The current locking design prioritizes correctness and simplicity over maximum throughput.

## Types

```cpp
using OrderId = std::int64_t;
using TraderId = std::int64_t;
using Price = std::int64_t;
using Qty = std::int64_t;
```

Prices and quantities are integer values. The project does not yet enforce tick size, lot size, balances, or account risk.

## Order Entry

Regular limit orders:

```cpp
SubmitResult buy(TraderId traderId, OrderId orderId, Price price, Qty quantity);
SubmitResult sell(TraderId traderId, OrderId orderId, Price price, Qty quantity);
```

Market orders:

```cpp
SubmitResult marketBuy(TraderId traderId, OrderId orderId, Qty quantity);
SubmitResult marketSell(TraderId traderId, OrderId orderId, Qty quantity);
```

Immediate-or-cancel orders:

```cpp
SubmitResult iocBuy(TraderId traderId, OrderId orderId, Price price, Qty quantity);
SubmitResult iocSell(TraderId traderId, OrderId orderId, Price price, Qty quantity);
```

Fill-or-kill orders:

```cpp
SubmitResult fokBuy(TraderId traderId, OrderId orderId, Price price, Qty quantity);
SubmitResult fokSell(TraderId traderId, OrderId orderId, Price price, Qty quantity);
```

Replace existing resting orders:

```cpp
SubmitResult replaceBuy(TraderId traderId, OrderId orderId, Price price, Qty quantity);
SubmitResult replaceSell(TraderId traderId, OrderId orderId, Price price, Qty quantity);
```

Cancel:

```cpp
bool cancel(OrderId orderId);
```

Generic lower-level API:

```cpp
SubmitResult submit(order incoming, Type type = Type::Regular);
SubmitResult replace(order replacement, Type type = Type::Regular);
```

Prefer the named methods for CLI/API/server code. They make it clear which operation is being requested.

## Result Object

Every order-entry method returns:

```cpp
struct SubmitResult {
    bool accepted;
    Qty filledQuantity;
    Qty restingQuantity;
    std::int64_t notional;
    bool selfTradePrevented;
    std::string message;
    std::vector<Trade> trades;

    double averagePrice() const;
};
```

`accepted` means the operation was accepted by the orderbook. An accepted order may still fill `0` quantity, for example an IOC order with no available liquidity.

`filledQuantity` is the total quantity traded by this operation.

`restingQuantity` is the quantity left on the book after this operation. Market, IOC, and FOK orders never rest leftover quantity.

`notional` is the sum of `trade price * trade quantity`.

`averagePrice()` returns `notional / filledQuantity`, or `0.0` if nothing filled.

`selfTradePrevented` is true when the orderbook stopped matching because the incoming order would trade with the same `TraderId`.

`message` is a short human-readable status string.

`trades` contains one entry per match:

```cpp
struct Trade {
    OrderId takerId;
    OrderId makerId;
    TraderId takerTraderId;
    TraderId makerTraderId;
    Side takerSide;
    Price price;
    Qty quantity;
};
```

## Behavior Rules

The orderbook uses price-time priority:

- best price matches first
- FIFO within a price level

Trade price is the resting order's price.

Regular limit orders:

- match immediately if possible
- rest any unfilled remainder

Market orders:

- match immediately against available opposite-side liquidity
- discard unfilled remainder

IOC orders:

- match immediately
- discard unfilled remainder

FOK orders:

- execute only if the full quantity can fill immediately
- otherwise reject without changing the book

Duplicate order IDs:

- `submit(...)`, `buy(...)`, `sell(...)`, etc. reject a duplicate resting `OrderId`
- use `replace(...)`, `replaceBuy(...)`, or `replaceSell(...)` to update an existing resting order

Self-trade prevention:

- orders carry a `TraderId`
- if an incoming order would trade against the same `TraderId`, the incoming remainder is canceled
- FOK liquidity checks do not count same-trader orders

## Query Methods

```cpp
bool empty() const;

Qty totalBuyQuantity() const;
Qty totalSellQuantity() const;
Qty totalQuantityAtPrice(Side side, Price price) const;

bool hasBestBid() const;
bool hasBestAsk() const;
Price bestBid() const;
Price bestAsk() const;

void print(std::ostream& os) const;
```

`bestBid()` and `bestAsk()` return `0` if that side of the book is empty. Use `hasBestBid()` and `hasBestAsk()` when `0` could be ambiguous.

## Local HTTP API

The project now includes a small local HTTP server for testing GET/POST calls around the C++ API.

See [HTTP_API.md](HTTP_API.md).

## CLI/HTTP Command Mapping

A CLI or network API can map each command to one method:

```text
BUY traderId orderId price quantity          -> book.buy(...)
SELL traderId orderId price quantity         -> book.sell(...)
MARKET_BUY traderId orderId quantity         -> book.marketBuy(...)
MARKET_SELL traderId orderId quantity        -> book.marketSell(...)
IOC_BUY traderId orderId price quantity      -> book.iocBuy(...)
IOC_SELL traderId orderId price quantity     -> book.iocSell(...)
FOK_BUY traderId orderId price quantity      -> book.fokBuy(...)
FOK_SELL traderId orderId price quantity     -> book.fokSell(...)
REPLACE_BUY traderId orderId price quantity  -> book.replaceBuy(...)
REPLACE_SELL traderId orderId price quantity -> book.replaceSell(...)
CANCEL orderId                               -> book.cancel(...)
PRINT                                        -> book.print(...)
```

Example:

```text
IOC_BUY 7 1001 99 10
```

should call:

```cpp
book.iocBuy(7, 1001, 99, 10);
```
