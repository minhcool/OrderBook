# Orderbook

A small C++20 limit order book / matching-engine project.

The project currently provides an in-memory orderbook library with:

- price-time FIFO matching
- buy and sell limit orders
- market orders
- IOC and FOK orders
- explicit order replace/update
- cancel by order ID
- trader IDs and self-trade prevention
- Clerk login support in the website
- coarse-grained mutex protection
- correctness tests
- performance benchmarks

It is not connected to a real exchange or database yet. The current API and website are development prototypes; all orderbook state is in memory and disappears when the API process restarts.

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

## Current Limitations

- No CLI yet.
- Local HTTP API exists for testing, but it is not production-grade.
- No WebSocket/FIX API yet.
- Website uses Clerk login, but account storage/roles are not implemented yet.
- Backend JWT signature verification is not implemented yet; the current API auth bridge is for development.
- No accounts, balances, margin, or settlement.
- No persistence or event log.
- No tick size or lot size validation.
- Cancel and replace still scan the book instead of using an order ID index.
- Mutex locking is correct but coarse-grained.
