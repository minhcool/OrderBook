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

- Runtime order books are in memory while the process is running. PostgreSQL records a single ordered replay log plus user/session/order/trade/cancel/rating history tables when `DATABASE_URL` is set.
- On startup, the API replays `orderbook_events` to restore sessions, lobbies, open orders, fills, cash/positions, market prices, cooldowns, and order ID generators.
- Checkpoint rows are currently replay watermarks. Full snapshot fast-forwarding from checkpoints is not implemented yet.
- One symbol maps to one orderbook inside one active room/lobby session.
- Public endpoints list rooms and lobby capacity/status only. Symbols, books, prices, trades, orders, and account data require entering the selected room/lobby first.
- Each authenticated user can have exactly one active session. Leaving cancels resting orders and starts a rejoin cooldown, 60 seconds by default.
- Single-player routes use `/rooms/{roomId}/...`; competitive market routes use `/lobbies/{lobbyId}/...`.
- A competitive room has multiple independent lobbies. Each lobby owns its own order books, trade history, accounts, participant capacity, timer, and lifecycle.
- Competitive lobby phases are `waiting`, `starting`, `running`, and `finished`. New players can join only while the lobby is `waiting` or `starting`; orders are accepted only while a joined lobby is `running`.
- New order IDs are assigned by the server. Clients pass `orderId` only when replacing or canceling an existing order.
- Cash and position checks reject overspending and selling shares the user does not own.
- Price marks are based on the room/lobby trade tape. The current portfolio mark uses the last trade price; `/prices` also exposes VWAPs over the last 3, 5, and 10 trades.
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
GET /me/session
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
      "startDelaySeconds": 0,
      "gameDurationSeconds": 600,
      "rejoinCooldownSeconds": 60,
      "houseLiquidity": true,
      "assets": []
    }
  ]
}
```

Public room responses intentionally hide assets. Enter the room/lobby and call `/symbols` to see tradable symbols. The asset `source` may say `masked-real-series`, but the API does not reveal the real-world backing symbol.

Enter or exit a single-player room:

```http
GET  /rooms/{roomId}/membership
POST /rooms/{roomId}/join
POST /rooms/{roomId}/leave
```

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
      "phase": "waiting",
      "participantCount": 0,
      "capacity": 10,
      "spotsRemaining": 10,
      "minStartParticipants": 5,
      "startDelaySeconds": 90,
      "gameDurationSeconds": 600,
      "startsAt": null,
      "endsAt": null,
      "startsInSeconds": 0,
      "endsInSeconds": 0
    }
  ]
}
```

Joining and leaving require a Clerk bearer token:

```http
GET  /lobbies/{lobbyId}/membership
POST /lobbies/{lobbyId}/join
POST /lobbies/{lobbyId}/leave
GET  /lobbies/{lobbyId}/leaderboard
```

Lobby `status` is `open`, `full`, or `closed`. `closed` means the game is already running or finished, so new users cannot enter. A previously admitted player may re-enter a running lobby after their cooldown if there is still an active seat available. Joining is idempotent. A full lobby, closed lobby, active session elsewhere, or cooldown returns `409`. Leaving cancels that trader's resting orders in the lobby. Market data, orders, and `/me` endpoints return `403` until the authenticated trader enters the selected room/lobby.

Lobby join accepts an optional body:

```json
{ "track": "manual" }
```

Bots should send `{ "track": "bot" }` so their Elo stays separate from manual users.

Leaderboard rows are available after a competitive lobby finishes. PnL is based on final estimated portfolio value versus starting cash. Manual users and bots are rated separately with a chess-style Elo update:

```json
{
  "leaderboard": [
    {
      "rank": 1,
      "traderId": 194214855,
      "track": "manual",
      "pnl": 250,
      "estimatedValue": 100250,
      "ratingBefore": 1200,
      "ratingAfter": 1216
    }
  ]
}
```

Known symbols:

```http
GET /rooms/{roomId}/symbols
GET /lobbies/{lobbyId}/symbols
```

Response:

```json
{ "symbols": ["BTC-USD", "ETH-USD"] }
```

Advance a single-player simulator:

```http
POST /rooms/{roomId}/simulator/tick
```

Body:

```json
{ "steps": 1 }
```

This requires entering the single-player room first. `steps` is clamped to `1..25`. Each tick moves hidden fair values, refreshes house liquidity, and can fill the user's resting orders if simulated quotes cross them.

Response:

```json
{
  "advanced": true,
  "steps": 1,
  "trades": 0,
  "symbols": ["NOVA", "ORBIT", "LYRA", "MIST"]
}
```

Market prices:

```http
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
GET /rooms/{roomId}/trades/{symbol}?limit=50
GET /lobbies/{lobbyId}/trades/{symbol}?limit=50
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
GET /rooms/{roomId}/book/{symbol}
GET /lobbies/{lobbyId}/book/{symbol}
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
  "cash": 99393,
  "reservedCash": 0,
  "availableCash": 99393,
  "reservedLongQuantity": 0,
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

Every order endpoint must be scoped to an entered room or lobby:

```text
/rooms/{roomId}/orders/...
/lobbies/{lobbyId}/orders/...
```

Regular limit orders:

```http
POST /rooms/{roomId}/orders/buy
POST /rooms/{roomId}/orders/sell
POST /lobbies/{lobbyId}/orders/buy
POST /lobbies/{lobbyId}/orders/sell
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
POST /rooms/{roomId}/orders/market-buy
POST /rooms/{roomId}/orders/market-sell
POST /lobbies/{lobbyId}/orders/market-buy
POST /lobbies/{lobbyId}/orders/market-sell
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
POST /rooms/{roomId}/orders/ioc-buy
POST /rooms/{roomId}/orders/ioc-sell
POST /lobbies/{lobbyId}/orders/ioc-buy
POST /lobbies/{lobbyId}/orders/ioc-sell
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
POST /rooms/{roomId}/orders/fok-buy
POST /rooms/{roomId}/orders/fok-sell
POST /lobbies/{lobbyId}/orders/fok-buy
POST /lobbies/{lobbyId}/orders/fok-sell
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
POST /rooms/{roomId}/orders/replace-buy
POST /rooms/{roomId}/orders/replace-sell
POST /lobbies/{lobbyId}/orders/replace-buy
POST /lobbies/{lobbyId}/orders/replace-sell
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
POST /rooms/{roomId}/orders/cancel
POST /lobbies/{lobbyId}/orders/cancel
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
$env:CLERK_TOKEN = "paste_a_clerk_session_jwt_here"
$headers = @{ Authorization = "Bearer $env:CLERK_TOKEN" }
```

Enter solo mode:

```powershell
Invoke-RestMethod -Method Post -Uri http://localhost:8080/rooms/solo-alpha/join -Headers $headers -ContentType "application/json" -Body "{}"
```

Read allowed symbols:

```powershell
Invoke-RestMethod -Method Get -Uri http://localhost:8080/rooms/solo-alpha/symbols -Headers $headers
```

Buy from house liquidity:

```powershell
$body = @{ symbol = "NOVA"; quantity = 5 } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri http://localhost:8080/rooms/solo-alpha/orders/market-buy -Headers $headers -ContentType "application/json" -Body $body
```

Read the book:

```powershell
Invoke-RestMethod -Method Get -Uri http://localhost:8080/rooms/solo-alpha/book/NOVA -Headers $headers
```

Read your portfolio estimate:

```powershell
Invoke-RestMethod -Method Get -Uri http://localhost:8080/rooms/solo-alpha/me/portfolio -Headers $headers
```

Cancel:

```powershell
$order = @{ symbol = "NOVA"; price = 95; quantity = 1 } | ConvertTo-Json
$buy = Invoke-RestMethod -Method Post -Uri http://localhost:8080/rooms/solo-alpha/orders/buy -Headers $headers -ContentType "application/json" -Body $order
$body = @{ symbol = "NOVA"; orderId = $buy.orderId } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri http://localhost:8080/rooms/solo-alpha/orders/cancel -Headers $headers -ContentType "application/json" -Body $body
```
