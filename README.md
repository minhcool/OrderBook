# Orderbook

A C++20 limit order book and trading-game simulator.

The project currently provides an in-memory orderbook library with:

- price-time FIFO matching
- buy and sell limit orders
- market orders
- IOC and FOK orders
- explicit order replace/update
- cancel by order ID
- trader IDs and self-trade prevention
- Clerk login support in the website
- server-assigned HTTP order IDs
- authenticated open-order, fill-history, and derived-position endpoints
- room/lobby trade tape, last-trade marks, and simple portfolio valuation
- single-player and competitive game rooms
- multiple competitive lobbies with independent books and enforced capacities
- authenticated room/lobby enter and exit
- one active session per user
- one-minute leave/rejoin cooldown by default
- waiting/starting/running/finished competitive lobby lifecycle
- starting cash, available cash, reserved cash, and no-short/no-overspend checks
- manual and bot tracks with chess-style Elo updates at game end
- optional PostgreSQL event/history persistence through `DATABASE_URL`
- fictional room assets, including synthetic and masked-real-series profiles
- room-scoped order books and portfolios
- coarse-grained mutex protection
- correctness tests
- performance benchmarks

It is not connected to a real exchange. The current API and website are development prototypes. Runtime order books are still held in memory, while PostgreSQL records users, session events, orders, cancels, trades, and ratings when `DATABASE_URL` is configured.

The game direction is:

```text
Trading engine
  -> rooms
  -> fictional assets
  -> human users, bots, and simulated liquidity
  -> portfolio valuation and leaderboard-ready scoring
```

## Layout

```text
include/orderbook/     public headers
src/                   orderbook implementation
tests/                 correctness tests
examples/              small hardcoded demo
benchmarks/            performance benchmark harness
docs/                  API reference
```

## Requirements

- C++20 compiler, tested with `g++`
- `mingw32-make` on Windows

## Quickstart

From PowerShell:

```powershell
cd C:\Users\nqmin\Documents\Projects\Orderbook
mingw32-make test
```

Run the demo:

```powershell
mingw32-make demo
```

Run performance benchmarks:

```powershell
mingw32-make perf
```

Build the local HTTP API server:

```powershell
mingw32-make api-build
```

Run the local HTTP API server:

```powershell
mingw32-make api
```

Run the website:

```powershell
cd web
npm install
Copy-Item .env.example .env
npm run dev
```

Run the competitive lobby API integration test:

```powershell
mingw32-make api-test
```

Remove generated build files:

```powershell
mingw32-make clean
```

## Minimal Example

```cpp
#include "orderbook/orderbook.h"

#include <iostream>

int main() {
    orderbook book;

    book.sell(10, 1, 100, 5);              // trader 10, order 1, price 100, qty 5
    SubmitResult result = book.buy(20, 2, 100, 3);

    std::cout << "filled=" << result.filledQuantity
              << " avg=" << result.averagePrice() << "\n";

    book.print(std::cout);
}
```

Compile manually:

```powershell
g++ -std=c++20 -Wall -Wextra -pedantic -Iinclude examples/demo.cpp src/orderbook.cpp -o build/orderbook_demo.exe
```

## API Docs

See [docs/API.md](docs/API.md).

For local HTTP GET/POST endpoints, see [docs/HTTP_API.md](docs/HTTP_API.md).

For the Vercel-ready website, see [docs/WEBSITE.md](docs/WEBSITE.md).

For deploying the C++ API server, see [docs/DEPLOY_API.md](docs/DEPLOY_API.md).

## Current Limitations

- No CLI yet.
- Local HTTP API exists for testing, but it is not production-grade.
- No WebSocket/FIX API yet.
- Website uses Clerk login, but roles are not implemented yet.
- Backend JWT signature verification is not implemented yet; the current API auth bridge is for development.
- Runtime books/account state are not rehydrated from PostgreSQL on restart yet.
- No deposits, withdrawals, margin, or settlement.
- Portfolio values are estimates from starting cash, fill cash flow, reserved cash, and in-memory last-trade marks.
- Competitive lobbies are currently seeded at startup; dynamic lobby creation and matchmaking are not implemented yet.
- No news engine or bot tournament scheduler yet.
- Masked-real-series assets are only metadata placeholders until a licensed historical data import exists.
- No tick size or lot size validation.
- Cancel and replace still scan the book instead of using an order ID index.
- Mutex locking is correct but coarse-grained.
