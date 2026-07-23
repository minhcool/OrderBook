# Deploy The API

The Vercel site is only the frontend. The orderbook API is a long-running C++ process, so deploy it as a web service/container.

## Recommended First Host: Render

This repo includes:

```text
Dockerfile
render.yaml
```

Render can build the Docker image from GitHub and run it as a public web service.

The included `render.yaml` uses Render's free web service plan for the prototype and defines a Render Postgres database named `orderbook-db`. The API receives `DATABASE_URL` from that database, writes a replayable event log plus user/session/order/trade/rating history, and rebuilds in-memory state from the event log on startup. Render free Postgres is useful for a prototype, but it has free-tier limits and expiration.

## Steps

1. Push the repo to GitHub.
2. Open Render and create a new Blueprint or Web Service from the GitHub repo.
3. If Render asks for a runtime, choose Docker.
4. Use the repo root as the service root.
5. Add the Clerk JWT public key when Render prompts for `CLERK_JWT_KEY`.
6. Add an `ORDERBOOK_ADMIN_TOKEN` value.
7. Optionally add `ORDERBOOK_BOT_KEYS` if deployed bots should trade without a Clerk browser session.
8. Let Render build and deploy.
9. After deploy, copy the public service URL.
10. In Render's Blueprint sync page, confirm both `orderbook-api` and `orderbook-db` exist.

`CLERK_JWT_KEY` is the JWT public key from Clerk Dashboard -> API keys. It starts with:

```text
-----BEGIN PUBLIC KEY-----
```

Do not use the Clerk publishable key (`pk_...`) here. The API uses this public key to verify signed Clerk session tokens.

Optional Render environment variables:

```text
CLERK_ISSUER=https://your-clerk-issuer
CLERK_AUTHORIZED_PARTIES=https://your-vercel-app.vercel.app
CLERK_AUDIENCE=your-api-audience
ORDERBOOK_BOT_KEYS=maker-bot:long_random_secret,trend-bot:another_secret
ORDERBOOK_RATE_LIMIT_PER_MINUTE=240
ORDERBOOK_MUTATION_RATE_LIMIT_PER_MINUTE=80
ORDERBOOK_LIVE_WAIT_MS=2500
```

Required for admin endpoints:

```text
ORDERBOOK_ADMIN_TOKEN=long_random_admin_secret
```

The API exposes:

```text
GET /health
GET /ready
GET /metrics
GET /rooms
GET /rooms/comp-aurora/lobbies
POST /rooms/solo-alpha/join
GET /rooms/solo-alpha/symbols
POST /rooms/solo-alpha/orders/market-buy
GET /admin/summary
```

## Verify

Replace the URL with your Render service URL:

```powershell
Invoke-RestMethod https://your-orderbook-api.onrender.com/health
Invoke-RestMethod https://your-orderbook-api.onrender.com/ready
Invoke-RestMethod https://your-orderbook-api.onrender.com/rooms
```

Expected health response:

```json
{ "ok": true }
```

Market data endpoints require a Clerk bearer token and an entered room/lobby. Public endpoints only expose room and lobby metadata.

Admin check:

```powershell
$headers = @{ "X-Admin-Token" = "paste_ORDERBOOK_ADMIN_TOKEN_here" }
Invoke-RestMethod https://your-orderbook-api.onrender.com/admin/summary -Headers $headers
```

## If Render Fails

Open the `orderbook-api` service, then check:

```text
Events -> latest failed deploy -> Logs
```

The Blueprint page only says that deploy failed. The service logs show whether the failure happened while building the Docker image, starting the binary, or passing the `/health` check.

## Connect Vercel

In the Vercel frontend project, set:

```text
VITE_ORDERBOOK_API_URL=https://your-orderbook-api.onrender.com
```

Then redeploy the frontend.

The website also stores the API URL in browser local storage after you click Connect. If your browser keeps showing `http://localhost:8080`, paste the public API URL into the API URL field and click Connect.

## Important Limitations

- Runtime books are still in memory while the API process is running. PostgreSQL event replay restores them after restart for events created by this replay-log version.
- Checkpoint rows are replay watermarks right now; full state snapshot fast-forwarding is not implemented yet.
- Older history rows from before the replay-log migration are not enough to rebuild live state.
- Render free Postgres expires after its free-tier window; use a paid database before relying on durable history.
- Authenticated endpoints require verified Clerk JWTs unless `ORDERBOOK_ALLOW_UNVERIFIED_JWT=1` is explicitly set for local tests. Do not set that flag on Render.
- Bot API keys and admin tokens are env-configured secrets; rotate them manually if they leak.
- Rate limits are in-memory per API process.
- CORS is currently open for development.
- This is a prototype deployment path, not a production trading system.
