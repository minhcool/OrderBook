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
5. Let Render build and deploy.
6. After deploy, copy the public service URL.
7. In Render's Blueprint sync page, confirm both `orderbook-api` and `orderbook-db` exist.

The API exposes:

```text
GET /health
GET /rooms
GET /rooms/comp-aurora/lobbies
POST /rooms/solo-alpha/join
GET /rooms/solo-alpha/symbols
POST /rooms/solo-alpha/orders/market-buy
```

## Verify

Replace the URL with your Render service URL:

```powershell
Invoke-RestMethod https://your-orderbook-api.onrender.com/health
Invoke-RestMethod https://your-orderbook-api.onrender.com/rooms
```

Expected health response:

```json
{ "ok": true }
```

Market data endpoints require a Clerk bearer token and an entered room/lobby. Public endpoints only expose room and lobby metadata.

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
- The backend reads the Clerk token subject but does not verify the JWT signature yet.
- CORS is currently open for development.
- This is a prototype deployment path, not a production trading system.
