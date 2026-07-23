# Website

The website is a Vite/React app in `web/`. It is a browser UI for the local HTTP API server.

Authentication is handled by Clerk. Signed-in users can submit, replace, and cancel orders. The website sends a Clerk bearer token to the C++ API server.

New order IDs are assigned by the API server. The website shows returned IDs in activity and uses them for replace/cancel actions.

Signed-in users can also see their account panel: current open orders, recent fills/trades, positions derived from those fills, cash flow, marked market value, estimated value, and unrealized PnL.

## Local Run

Terminal 1:

```powershell
cd C:\Users\nqmin\Documents\Projects\Orderbook
mingw32-make api
```

Terminal 2:

```powershell
cd C:\Users\nqmin\Documents\Projects\Orderbook\web
npm install
npm run dev
```

Open:

```text
http://127.0.0.1:5173
```

The default API URL is:

```text
http://localhost:8080
```

You can change it in the website UI.

## Vercel Deploy

The repo root contains `vercel.json`, configured to build the `web/` app and serve `web/dist`.

On Vercel:

1. Import the repository.
2. Use the repository root as the project root.
3. Keep the build settings from `vercel.json`.
4. Set `VITE_ORDERBOOK_API_URL` to the public URL of the orderbook API server.
5. Set `VITE_CLERK_PUBLISHABLE_KEY` to your Clerk publishable key.

Important: Vercel should host the frontend first. The current C++ API server is stateful and should run separately on a server/VPS/container that can keep one process alive.

For API deployment steps, see [DEPLOY_API.md](DEPLOY_API.md).

## API URL

For local development:

```text
VITE_ORDERBOOK_API_URL=http://localhost:8080
VITE_CLERK_PUBLISHABLE_KEY=pk_test_...
```

For deployment:

```text
VITE_ORDERBOOK_API_URL=https://your-api-server.example.com
VITE_CLERK_PUBLISHABLE_KEY=pk_live_...
```

The C++ API server now includes basic CORS headers for browser requests.

## Account Data

The account panel reads these authenticated endpoints:

```text
GET /me
GET /me/orders
GET /me/fills
GET /me/positions
GET /me/portfolio
```

The market strip reads:

```text
GET /prices/{symbol}
```

This data is currently in memory on the C++ API server. It is enough to test a real user flow, but it is not durable yet: restarting or redeploying the API clears the books, trade tape, fills, derived positions, and portfolio marks.

Portfolio value is estimated from trade cash flow and last-trade marks. It is not a real account balance yet because deposits, withdrawals, reserves, and starting balances are not implemented.

## Clerk Setup

1. Create a Clerk application.
2. Enable the OAuth providers you want, such as Google or GitHub.
3. Run `clerk auth login`.
4. Run `clerk init --app app_3GqtszmnEQINzeFX8CUrXDhCXXz` from `web/`.
5. Keep the generated `web/.env.local` local; it is intentionally ignored by Git.
6. Add the publishable key in Vercel as `VITE_CLERK_PUBLISHABLE_KEY`.

The current backend reads the Clerk token subject and maps it to an internal `TraderId`. It still needs real Clerk JWT verification before production use.
