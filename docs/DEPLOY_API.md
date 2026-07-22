# Deploy The API

The Vercel site is only the frontend. The orderbook API is a long-running C++ process, so deploy it as a web service/container.

## Recommended First Host: Render

This repo includes:

```text
Dockerfile
render.yaml
```

Render can build the Docker image from GitHub and run it as a public web service.

The included `render.yaml` uses Render's free web service plan for the prototype. A free service can restart, so the in-memory book can clear at any time.

## Steps

1. Push the repo to GitHub.
2. Open Render and create a new Blueprint or Web Service from the GitHub repo.
3. If Render asks for a runtime, choose Docker.
4. Use the repo root as the service root.
5. Let Render build and deploy.
6. After deploy, copy the public service URL.

The API exposes:

```text
GET /health
GET /symbols
GET /book/BTC-USD
POST /orders/buy
POST /orders/sell
```

## Verify

Replace the URL with your Render service URL:

```powershell
Invoke-RestMethod https://your-orderbook-api.onrender.com/health
Invoke-RestMethod https://your-orderbook-api.onrender.com/book/BTC-USD
```

Expected health response:

```json
{ "ok": true }
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

- The API still stores all books in memory. Restarting the service clears the books.
- The backend reads the Clerk token subject but does not verify the JWT signature yet.
- CORS is currently open for development.
- This is a prototype deployment path, not a production trading system.
