# Local HTTP API

The local API server is a thin wrapper around the C++ `Exchange` and `orderbook` APIs. It is meant for development/testing, not production.

Run it:

```powershell
cd C:\Users\nqmin\Documents\Projects\Orderbook
mingw32-make api
```

Default address:

```text
http://localhost:8080
```

Run on another port:

```powershell
.\build\orderbook_api.exe 18080
```

Stop it with `Ctrl+C`.

## Notes

- The server stores all state in memory.
- Restarting the server clears all books.
- One symbol maps to one orderbook.
- Request bodies are flat JSON objects.
- The parser is intentionally tiny and only supports the fields shown here.

## GET Endpoints

Health:

```http
GET /health
```

Response:

```json
{ "ok": true }
```

Known symbols:

```http
GET /symbols
```

Response:

```json
{ "symbols": ["BTC-USD", "ETH-USD"] }
```

Book snapshot:

```http
GET /book/{symbol}
GET /book/{symbol}?depth=5
```

Response:

```json
{
  "symbol": "BTC-USD",
  "bids": [
    { "price": 99, "quantity": 3 }
  ],
  "asks": [
    { "price": 100, "quantity": 2 }
  ]
}
```

## POST Endpoints

Regular limit orders:

```http
POST /orders/buy
POST /orders/sell
```

Body:

```json
{
  "symbol": "BTC-USD",
  "traderId": 1,
  "orderId": 101,
  "price": 100,
  "quantity": 5
}
```

Market orders:

```http
POST /orders/market-buy
POST /orders/market-sell
```

Body:

```json
{
  "symbol": "BTC-USD",
  "traderId": 1,
  "orderId": 102,
  "quantity": 5
}
```

IOC orders:

```http
POST /orders/ioc-buy
POST /orders/ioc-sell
```

Body:

```json
{
  "symbol": "BTC-USD",
  "traderId": 1,
  "orderId": 103,
  "price": 100,
  "quantity": 5
}
```

FOK orders:

```http
POST /orders/fok-buy
POST /orders/fok-sell
```

Body:

```json
{
  "symbol": "BTC-USD",
  "traderId": 1,
  "orderId": 104,
  "price": 100,
  "quantity": 5
}
```

Replace resting orders:

```http
POST /orders/replace-buy
POST /orders/replace-sell
```

Body:

```json
{
  "symbol": "BTC-USD",
  "traderId": 1,
  "orderId": 101,
  "price": 101,
  "quantity": 10
}
```

Cancel:

```http
POST /orders/cancel
```

Body:

```json
{
  "symbol": "BTC-USD",
  "orderId": 101
}
```

## Order Response

Most order endpoints return:

```json
{
  "accepted": true,
  "filledQuantity": 3,
  "restingQuantity": 0,
  "notional": 300,
  "averagePrice": 100,
  "selfTradePrevented": false,
  "message": "traded",
  "trades": [
    {
      "takerId": 201,
      "makerId": 101,
      "takerTraderId": 2,
      "makerTraderId": 1,
      "takerSide": "buy",
      "price": 100,
      "quantity": 3
    }
  ]
}
```

Cancel returns:

```json
{ "canceled": true }
```

## PowerShell Examples

PowerShell can mangle JSON quoting for `curl.exe`. Use `--%` in these examples.

Place a sell:

```powershell
curl.exe --% -s -X POST http://localhost:8080/orders/sell -H "Content-Type: application/json" -d "{\"symbol\":\"BTC-USD\",\"traderId\":1,\"orderId\":101,\"price\":100,\"quantity\":5}"
```

Place a crossing buy:

```powershell
curl.exe --% -s -X POST http://localhost:8080/orders/buy -H "Content-Type: application/json" -d "{\"symbol\":\"BTC-USD\",\"traderId\":2,\"orderId\":201,\"price\":100,\"quantity\":3}"
```

Read the book:

```powershell
curl.exe -s http://localhost:8080/book/BTC-USD
```

Cancel:

```powershell
curl.exe --% -s -X POST http://localhost:8080/orders/cancel -H "Content-Type: application/json" -d "{\"symbol\":\"BTC-USD\",\"orderId\":101}"
```
