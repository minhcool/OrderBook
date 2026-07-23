# Bot API

Bots use the same HTTP API as the website. For now, authentication is a Clerk session JWT in:

```http
Authorization: Bearer <token>
```

The backend derives the internal `traderId` from the token's `sub` claim. JWT signature verification is still a prototype limitation, so do not treat this as production security yet.

## Competitive Bot Flow

1. List rooms and lobbies:

```http
GET /rooms
GET /rooms/comp-aurora/lobbies
```

2. Join a lobby on the bot track:

```http
POST /lobbies/{lobbyId}/join
Content-Type: application/json

{ "track": "bot" }
```

3. Poll market/account state:

```http
GET /lobbies/{lobbyId}/symbols
GET /lobbies/{lobbyId}/book/{symbol}?depth=10
GET /lobbies/{lobbyId}/prices/{symbol}
GET /lobbies/{lobbyId}/me/portfolio
GET /lobbies/{lobbyId}/me/orders
GET /lobbies/{lobbyId}/me/fills
```

4. Trade with the same order endpoints:

```http
POST /lobbies/{lobbyId}/orders/buy
POST /lobbies/{lobbyId}/orders/sell
POST /lobbies/{lobbyId}/orders/market-buy
POST /lobbies/{lobbyId}/orders/market-sell
POST /lobbies/{lobbyId}/orders/ioc-buy
POST /lobbies/{lobbyId}/orders/ioc-sell
POST /lobbies/{lobbyId}/orders/fok-buy
POST /lobbies/{lobbyId}/orders/fok-sell
POST /lobbies/{lobbyId}/orders/replace-buy
POST /lobbies/{lobbyId}/orders/replace-sell
POST /lobbies/{lobbyId}/orders/cancel
```

New order IDs are assigned by the server. Bots only send `orderId` for replace/cancel.

## Single-Player Bot Flow

Single-player is useful for local bot development because every bot gets a personal simulated market:

```http
POST /rooms/solo-alpha/join
POST /rooms/solo-alpha/simulator/tick
GET  /rooms/solo-alpha/book/NOVA
POST /rooms/solo-alpha/orders/buy
GET  /rooms/solo-alpha/me/portfolio
```

`/simulator/tick` advances hidden fair values, refreshes house liquidity, and can fill resting bot orders. Use small polling intervals, such as 1-2 seconds, while testing.

## Minimal PowerShell Example

```powershell
$api = "https://your-orderbook-api.onrender.com"
$headers = @{ Authorization = "Bearer $env:CLERK_TOKEN" }

Invoke-RestMethod -Method Post -Uri "$api/lobbies/aurora-open-10/join" `
  -Headers $headers -ContentType "application/json" -Body '{"track":"bot"}'

$book = Invoke-RestMethod -Uri "$api/lobbies/aurora-open-10/book/AXON?depth=5" -Headers $headers
$price = $book.bids[0].price

$order = @{ symbol = "AXON"; price = $price; quantity = 1 } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri "$api/lobbies/aurora-open-10/orders/buy" `
  -Headers $headers -ContentType "application/json" -Body $order
```

## Current Limits

- No WebSocket stream yet; bots should poll.
- No API-key auth yet; bots currently need a Clerk session token.
- Competitive bots must join before the lobby starts unless they were already admitted and are rejoining after cooldown.
- Rate limits and bot tournament scheduling are not implemented yet.
