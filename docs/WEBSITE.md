# Website

The website is a Vite/React app in `web/`. It is a browser UI for the local HTTP API server.

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

Important: Vercel should host the frontend first. The current C++ API server is stateful and should run separately on a server/VPS/container that can keep one process alive.

## API URL

For local development:

```text
VITE_ORDERBOOK_API_URL=http://localhost:8080
```

For deployment:

```text
VITE_ORDERBOOK_API_URL=https://your-api-server.example.com
```

The C++ API server now includes basic CORS headers for browser requests.
