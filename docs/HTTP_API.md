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
- The API server starts with a backward-compatible `sandbox` room and game definitions for single-player and competitive trading.
- A competitive room has multiple independent lobbies. Each lobby owns its own order books, trade history, accounts, and participant capacity.
- Old routes like `/book/BTC-USD` use the `sandbox` room. Single-player routes use `/rooms/{roomId}/...`; competitive market routes use `/lobbies/{lobbyId}/...`.
- New order IDs are assigned by the server. Clients pass `orderId` only when replacing or canceling an existing order.
- Open orders, fills, and positions are also stored/read in memory.
- Price marks are based on the public trade tape. The current portfolio mark uses the last trade price; `/prices` also exposes VWAPs over the last 3, 5, and 10 trades.
- Request bodies are flat JSON objects.
- The parser is intentionally tiny and only supports the fields shown here.
- POST order endpoints and `/me` endpoints require `Authorization: Bearer <Clerk session token>`.
- Missing or malformed bearer tokens return `401`.
- The current C++ server derives `TraderId` from the Clerk JWT `sub` claim, but does not yet verify the JWT signature against Clerk JWKS. Add real JWT verification before making trading public.

## GET Endpoints

Health:

```http
GET /health
```

Response:

```json
{ "ok": true }
```

Rooms:

```http
GET /rooms
GET /rooms/{roomId}
```

Response:

```json
{
  "rooms": [
    {
      "id": "solo-alpha",
      "name": "Solo Alpha Lab",
      "mode": "single",
      "difficulty": "medium",
      "startingCash": 100000,
      "maxParticipants": 1,
      "houseLiquidity": true,
      "assets": [
        {
          "symbol": "NOVA",
          "displayName": "Nova Systems",
          "behavior": "trend-cycle",
          "source": "synthetic",
          "referencePrice": 100,
          "signalQuality": "learnable"
        }
      ]
    }
  ]
}
```

The asset `source` may say `masked-real-series`, but the API does not reveal the real-world backing symbol.

Competitive lobbies:

```http
GET /lobbies
GET /lobbies/{lobbyId}
GET /rooms/{roomId}/lobbies
```

Response:

```json
{
  "lobbies": [
    {
      "id": "aurora-open-10",
      "name": "Aurora Open 10",
      "roomId": "comp-aurora",
      "status": "open",
      "participantCount": 0,
      "capacity": 10,
      "spotsRemaining": 10
    }
  ]
}
```

Joining and leaving require a Clerk bearer token:

```http
GET  /lobbies/{lobbyId}/membership
POST /lobbies/{lobbyId}/join
POST /lobbies/{lobbyId}/leave
```

Joining is idempotent. A full lobby returns `409`. Leaving cancels that trader's resting orders in the lobby. Public books and market data can be inspected without joining, while order and `/me` endpoints return `403` until the authenticated trader joins.

Known symbols:

```http
GET /symbols
GET /rooms/{roomId}/symbols
GET /lobbies/{lobbyId}/symbols
```

Response:

```json
{ "symbols": ["BTC-USD", "ETH-USD"] }
```

Market prices:

```http
GET /prices
GET /prices/{symbol}
GET /rooms/{roomId}/prices
GET /rooms/{roomId}/prices/{symbol}
GET /lobbies/{lobbyId}/prices
GET /lobbies/{lobbyId}/prices/{symbol}
```

Response:

```json
{
  "symbol": "BTC-USD",
  "tradeCount": 3,
  "hasPrice": true,
  "lastPrice": 105,
  "averagePrice3": 101.166667,
  "averagePrice5": 101.166667,
  "averagePrice10": 101.166667
}
```

`GET /prices` returns the same objects inside `{ "prices": [...] }`.

`averagePrice3`, `averagePrice5`, and `averagePrice10` are volume-weighted averages from the latest trades for that symbol.

Recent market trades:

```http
GET /trades/{symbol}
GET /trades/{symbol}?limit=50
GET /rooms/{roomId}/trades/{symbol}?limit=50
```

Response:

```json
{
  "trades": [
    {
      "sequence": 3,
      "symbol": "BTC-USD",
      "takerId": 202,
      "makerId": 201,
      "takerTraderId": 2,
      "makerTraderId": 1,
      "takerSide": "buy",
      "price": 105,
      "quantity": 1,
      "notional": 105
    }
  ]
}
```

Book snapshot:

```http
GET /book/{symbol}
GET /book/{symbol}?depth=5
GET /rooms/{roomId}/book/{symbol}
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

Authenticated account summary:

```http
GET /me
Authorization: Bearer <Clerk session token>
```

Response:

```json
{
  "traderId": 194214855,
  "openOrderCount": 1,
  "fillCount": 2,
  "positionCount": 1
}
```

Authenticated open orders:

```http
GET /me/orders
Authorization: Bearer <Clerk session token>
```

Response:

```json
{
  "orders": [
    {
      "symbol": "BTC-USD",
      "orderId": 101,
      "side": "sell",
      "price": 100,
      "quantity": 5,
      "remainingQuantity": 2
    }
  ]
}
```

Authenticated fills/trades:

```http
GET /me/fills
GET /me/trades
Authorization: Bearer <Clerk session token>
```

Response:

```json
{
  "fills": [
    {
      "sequence": 2,
      "symbol": "BTC-USD",
      "orderId": 201,
      "counterpartyOrderId": 101,
      "side": "buy",
      "liquidity": "taker",
      "price": 100,
      "quantity": 3,
      "notional": 300
    }
  ]
}
```

Authenticated positions:

```http
GET /me/positions
Authorization: Bearer <Clerk session token>
```

Positions are derived from fills only. Positive `quantity` means net bought base asset; negative means net sold. `quoteCashFlow` is positive when the trader received quote currency and negative when the trader spent it.

Response:

```json
{
  "positions": [
    {
      "symbol": "BTC-USD",
      "quantity": 3,
      "quoteCashFlow": -300,
      "boughtQuantity": 3,
      "soldQuantity": 0,
      "averageEntryPrice": 100
    }
  ]
}
```

Authenticated portfolio:

```http
GET /me/portfolio
Authorization: Bearer <Clerk session token>
```

`cashFlow` is quote currency received/spent from fills only. `tradingPnl` is the trading-derived estimate:

```text
cashFlow + sum(position quantity * last trade price)
```

`estimatedValue` adds the room's starting cash:

```text
startingCash + tradingPnl
```

Response:

```json
{
  "traderId": 194214855,
  "startingCash": 100000,
  "cashFlow": -607,
  "marketValue": 630,
  "estimatedValue": 100023,
  "tradingPnl": 23,
  "unrealizedPnl": 23,
  "positions": [
    {
      "symbol": "BTC-USD",
      "quantity": 6,
      "quoteCashFlow": -607,
      "boughtQuantity": 6,
      "soldQuantity": 0,
      "averageEntryPrice": 101.166667,
      "hasMark": true,
      "markPrice": 105,
      "marketValue": 630,
      "costBasisValue": 607,
      "unrealizedPnl": 23
    }
  ]
}
```

## POST Endpoints

Every order endpoint also works inside a game room:

```text
/rooms/{roomId}/orders/...
```

Regular limit orders:

```http
POST /orders/buy
POST /orders/sell
```

Body:

```json
{
  "symbol": "BTC-USD",
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
  "orderId": 201,
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

Set a token first:

```powershell
$env:CLERK_TOKEN_A = "paste_a_clerk_session_jwt_for_user_a_here"
$env:CLERK_TOKEN_B = "paste_a_clerk_session_jwt_for_user_b_here"
```

Place a sell:

```powershell
$sellerHeaders = @{ Authorization = "Bearer $env:CLERK_TOKEN_A" }
$body = @{ symbol = "BTC-USD"; price = 100; quantity = 5 } | ConvertTo-Json
$sell = Invoke-RestMethod -Method Post -Uri http://localhost:8080/orders/sell -Headers $sellerHeaders -ContentType "application/json" -Body $body
$sell.orderId
```

Place a crossing buy from a different user:

```powershell
$buyerHeaders = @{ Authorization = "Bearer $env:CLERK_TOKEN_B" }
$body = @{ symbol = "BTC-USD"; price = 100; quantity = 3 } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri http://localhost:8080/orders/buy -Headers $buyerHeaders -ContentType "application/json" -Body $body
```

Read the book:

```powershell
curl.exe -s http://localhost:8080/book/BTC-USD
```

Read your fills:

```powershell
Invoke-RestMethod -Method Get -Uri http://localhost:8080/me/fills -Headers $buyerHeaders
```

Read your portfolio estimate:

```powershell
Invoke-RestMethod -Method Get -Uri http://localhost:8080/me/portfolio -Headers $buyerHeaders
```

Cancel:

```powershell
$body = @{ symbol = "BTC-USD"; orderId = $sell.orderId } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri http://localhost:8080/orders/cancel -Headers $sellerHeaders -ContentType "application/json" -Body $body
```
