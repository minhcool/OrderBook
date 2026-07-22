import React from "react";
import ReactDOM from "react-dom/client";
import { ClerkProvider, Show, SignInButton, SignUpButton, UserButton, useAuth, useUser } from "@clerk/react";
import { Activity, CircleOff, Eraser, PlugZap, RefreshCw, Replace, Send, Server, Wifi, WifiOff } from "lucide-react";

import { cancelOrder, fetchBook, fetchSymbols, health, replaceOrder, submitOrder } from "./api";
import type { BookSnapshot, OrderMode, Side, SubmitResult } from "./types";
import "./styles.css";

const DEFAULT_API_BASE = import.meta.env.VITE_ORDERBOOK_API_URL ?? "http://localhost:8080";
const CLERK_PUBLISHABLE_KEY = import.meta.env.VITE_CLERK_PUBLISHABLE_KEY;
const DEFAULT_SYMBOL = "BTC-USD";

interface ActivityItem {
  id: number;
  title: string;
  detail: string;
  ok: boolean;
}

function toNumber(value: string): number {
  return Number.parseInt(value, 10);
}

function formatNumber(value: number): string {
  return Number.isFinite(value) ? value.toLocaleString("en-US") : "0";
}

function resultSummary(result: SubmitResult): string {
  const avg = result.filledQuantity > 0 ? ` avg ${result.averagePrice.toFixed(2)}` : "";
  return `${result.message}: filled ${result.filledQuantity}, resting ${result.restingQuantity}${avg}`;
}

function App() {
  const { getToken, isLoaded, isSignedIn } = useAuth();
  const { user } = useUser();

  const [apiBase, setApiBase] = React.useState(() => localStorage.getItem("orderbook.apiBase") ?? DEFAULT_API_BASE);
  const [symbol, setSymbol] = React.useState(DEFAULT_SYMBOL);
  const [symbols, setSymbols] = React.useState<string[]>([]);
  const [book, setBook] = React.useState<BookSnapshot>({ symbol: DEFAULT_SYMBOL, bids: [], asks: [] });
  const [apiOnline, setApiOnline] = React.useState(false);
  const [loading, setLoading] = React.useState(false);
  const [activity, setActivity] = React.useState<ActivityItem[]>([]);

  const [side, setSide] = React.useState<Side>("buy");
  const [mode, setMode] = React.useState<OrderMode>("limit");
  const [orderId, setOrderId] = React.useState("1001");
  const [price, setPrice] = React.useState("100");
  const [quantity, setQuantity] = React.useState("5");

  const [replaceSide, setReplaceSide] = React.useState<Side>("buy");
  const [replaceOrderId, setReplaceOrderId] = React.useState("1001");
  const [replacePriceValue, setReplacePriceValue] = React.useState("101");
  const [replaceQuantity, setReplaceQuantity] = React.useState("5");
  const [cancelId, setCancelId] = React.useState("1001");

  const pushActivity = React.useCallback((title: string, detail: string, ok: boolean) => {
    setActivity((items) => [{ id: Date.now() + Math.random(), title, detail, ok }, ...items].slice(0, 8));
  }, []);

  const requireToken = React.useCallback(async () => {
    if (!isLoaded || !isSignedIn) {
      throw new Error("Sign in before sending orders");
    }

    const token = await getToken();
    if (!token) {
      throw new Error("Could not read Clerk session token");
    }

    return token;
  }, [getToken, isLoaded, isSignedIn]);

  const refresh = React.useCallback(async () => {
    setLoading(true);
    localStorage.setItem("orderbook.apiBase", apiBase);

    try {
      await health(apiBase);
      const [nextBook, nextSymbols] = await Promise.all([
        fetchBook(apiBase, symbol, 10),
        fetchSymbols(apiBase)
      ]);

      setBook(nextBook);
      setSymbols(nextSymbols);
      setApiOnline(true);
    } catch (error) {
      setApiOnline(false);
      pushActivity("API offline", error instanceof Error ? error.message : "Request failed", false);
    } finally {
      setLoading(false);
    }
  }, [apiBase, pushActivity, symbol]);

  React.useEffect(() => {
    void refresh();
  }, [refresh]);

  async function handleSubmitOrder(event: React.FormEvent) {
    event.preventDefault();

    const payload = {
      symbol,
      orderId: toNumber(orderId),
      quantity: toNumber(quantity),
      ...(mode === "market" ? {} : { price: toNumber(price) })
    };

    try {
      const token = await requireToken();
      const result = await submitOrder(apiBase, token, side, mode, payload);
      pushActivity(`${mode.toUpperCase()} ${side.toUpperCase()} #${payload.orderId}`, resultSummary(result), result.accepted);
      setOrderId(String(toNumber(orderId) + 1));
      await refresh();
    } catch (error) {
      pushActivity("Order rejected", error instanceof Error ? error.message : "Request failed", false);
    }
  }

  async function handleReplace(event: React.FormEvent) {
    event.preventDefault();

    const payload = {
      symbol,
      orderId: toNumber(replaceOrderId),
      price: toNumber(replacePriceValue),
      quantity: toNumber(replaceQuantity)
    };

    try {
      const token = await requireToken();
      const result = await replaceOrder(apiBase, token, replaceSide, payload);
      pushActivity(`REPLACE ${replaceSide.toUpperCase()} #${payload.orderId}`, resultSummary(result), result.accepted);
      await refresh();
    } catch (error) {
      pushActivity("Replace failed", error instanceof Error ? error.message : "Request failed", false);
    }
  }

  async function handleCancel(event: React.FormEvent) {
    event.preventDefault();

    try {
      const token = await requireToken();
      const result = await cancelOrder(apiBase, token, symbol, toNumber(cancelId));
      pushActivity(`CANCEL #${cancelId}`, result.canceled ? "canceled" : "order not found", result.canceled);
      await refresh();
    } catch (error) {
      pushActivity("Cancel failed", error instanceof Error ? error.message : "Request failed", false);
    }
  }

  const spread = book.bids.length > 0 && book.asks.length > 0 ? book.asks[0].price - book.bids[0].price : null;
  const signedInName = user?.primaryEmailAddress?.emailAddress ?? user?.fullName ?? user?.id ?? "Signed in";

  return (
    <main className="shell">
      <header className="topbar">
        <div>
          <h1>Orderbook Console</h1>
          <p>{symbol}</p>
        </div>
        <div className="status-row">
          <Show when="signed-in">
            <span className="user-pill">{signedInName}</span>
            <UserButton />
          </Show>
          <Show when="signed-out">
            <SignInButton mode="modal">
              <button type="button">Sign in</button>
            </SignInButton>
            <SignUpButton mode="modal">
              <button type="button">Sign up</button>
            </SignUpButton>
          </Show>
          <span className={`status ${apiOnline ? "online" : "offline"}`}>
            {apiOnline ? <Wifi size={16} /> : <WifiOff size={16} />}
            {apiOnline ? "API online" : "API offline"}
          </span>
          <button className="icon-button" onClick={refresh} title="Refresh book" type="button">
            <RefreshCw size={18} className={loading ? "spin" : ""} />
          </button>
        </div>
      </header>

      <section className="connection-band">
        <label>
          API URL
          <input value={apiBase} onChange={(event) => setApiBase(event.target.value)} />
        </label>
        <label>
          Symbol
          <input value={symbol} onChange={(event) => setSymbol(event.target.value.toUpperCase())} />
        </label>
        <button type="button" onClick={refresh}>
          <PlugZap size={17} />
          Connect
        </button>
        <div className="symbols">
          {symbols.map((item) => (
            <button key={item} type="button" className={item === symbol ? "selected" : ""} onClick={() => setSymbol(item)}>
              {item}
            </button>
          ))}
        </div>
      </section>

      <section className="market-strip">
        <Metric label="Best bid" value={book.bids[0] ? formatNumber(book.bids[0].price) : "-"} />
        <Metric label="Best ask" value={book.asks[0] ? formatNumber(book.asks[0].price) : "-"} />
        <Metric label="Spread" value={spread === null ? "-" : formatNumber(spread)} />
        <Metric label="Depth" value={`${book.bids.length}/${book.asks.length}`} />
      </section>

      <div className="workspace">
        <section className="panel book-panel">
          <div className="panel-title">
            <Server size={18} />
            <h2>Book</h2>
          </div>
          <div className="book-grid">
            <BookSide title="Bids" levels={book.bids} tone="bid" />
            <BookSide title="Asks" levels={book.asks} tone="ask" />
          </div>
        </section>

        <section className="panel ticket-panel">
          <div className="panel-title">
            <Send size={18} />
            <h2>Order Ticket</h2>
          </div>

          <form className="form-grid" onSubmit={handleSubmitOrder}>
            <Show when="signed-out">
              <div className="auth-callout">
                <strong>Sign in required</strong>
                <span>Orders are tied to your Clerk user. Trader ID is assigned by the API.</span>
              </div>
            </Show>
            <Segmented<Side> value={side} onChange={setSide} options={[
              { label: "Buy", value: "buy" },
              { label: "Sell", value: "sell" }
            ]} />
            <Segmented<OrderMode> value={mode} onChange={setMode} options={[
              { label: "Limit", value: "limit" },
              { label: "Market", value: "market" },
              { label: "IOC", value: "ioc" },
              { label: "FOK", value: "fok" }
            ]} />
            <label>
              Order ID
              <input inputMode="numeric" value={orderId} onChange={(event) => setOrderId(event.target.value)} />
            </label>
            <label>
              Price
              <input inputMode="numeric" value={price} disabled={mode === "market"} onChange={(event) => setPrice(event.target.value)} />
            </label>
            <label>
              Quantity
              <input inputMode="numeric" value={quantity} onChange={(event) => setQuantity(event.target.value)} />
            </label>
            <button className={`primary ${side}`} type="submit" disabled={!isSignedIn}>
              <Send size={17} />
              Submit
            </button>
          </form>
        </section>

        <section className="panel manage-panel">
          <div className="panel-title">
            <Replace size={18} />
            <h2>Manage</h2>
          </div>

          <form className="form-grid compact" onSubmit={handleReplace}>
            <Show when="signed-out">
              <div className="auth-callout">
                <strong>Sign in required</strong>
                <span>Only authenticated users can replace or cancel orders.</span>
              </div>
            </Show>
            <Segmented<Side> value={replaceSide} onChange={setReplaceSide} options={[
              { label: "Buy", value: "buy" },
              { label: "Sell", value: "sell" }
            ]} />
            <label>
              Order ID
              <input inputMode="numeric" value={replaceOrderId} onChange={(event) => setReplaceOrderId(event.target.value)} />
            </label>
            <label>
              Price
              <input inputMode="numeric" value={replacePriceValue} onChange={(event) => setReplacePriceValue(event.target.value)} />
            </label>
            <label>
              Quantity
              <input inputMode="numeric" value={replaceQuantity} onChange={(event) => setReplaceQuantity(event.target.value)} />
            </label>
            <button type="submit" disabled={!isSignedIn}>
              <Replace size={17} />
              Replace
            </button>
          </form>

          <form className="cancel-row" onSubmit={handleCancel}>
            <label>
              Cancel ID
              <input inputMode="numeric" value={cancelId} onChange={(event) => setCancelId(event.target.value)} />
            </label>
            <button type="submit" className="danger" disabled={!isSignedIn}>
              <CircleOff size={17} />
              Cancel
            </button>
          </form>
        </section>

        <section className="panel activity-panel">
          <div className="panel-title">
            <Activity size={18} />
            <h2>Activity</h2>
            <button className="icon-button ghost" type="button" onClick={() => setActivity([])} title="Clear activity">
              <Eraser size={17} />
            </button>
          </div>
          <div className="activity-list">
            {activity.length === 0 ? (
              <div className="empty-state">No activity</div>
            ) : (
              activity.map((item) => (
                <div key={item.id} className={`activity-item ${item.ok ? "ok" : "bad"}`}>
                  <strong>{item.title}</strong>
                  <span>{item.detail}</span>
                </div>
              ))
            )}
          </div>
        </section>
      </div>
    </main>
  );
}

function Metric({ label, value }: { label: string; value: string }) {
  return (
    <div className="metric">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function BookSide({ title, levels, tone }: { title: string; levels: BookSnapshot["bids"]; tone: "bid" | "ask" }) {
  const maxQty = Math.max(1, ...levels.map((level) => level.quantity));

  return (
    <div className={`book-side ${tone}`}>
      <div className="book-head">
        <span>{title}</span>
        <span>Qty</span>
      </div>
      {levels.length === 0 ? (
        <div className="empty-state">Empty</div>
      ) : (
        levels.map((level) => (
          <div className="book-row" key={`${tone}-${level.price}`}>
            <div className="bar" style={{ width: `${Math.max(8, (level.quantity / maxQty) * 100)}%` }} />
            <strong>{formatNumber(level.price)}</strong>
            <span>{formatNumber(level.quantity)}</span>
          </div>
        ))
      )}
    </div>
  );
}

function Segmented<T extends string>({
  value,
  onChange,
  options
}: {
  value: T;
  onChange: (value: T) => void;
  options: Array<{ label: string; value: T }>;
}) {
  return (
    <div className="segmented">
      {options.map((option) => (
        <button
          key={option.value}
          type="button"
          className={option.value === value ? "active" : ""}
          onClick={() => onChange(option.value)}
        >
          {option.label}
        </button>
      ))}
    </div>
  );
}

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    {CLERK_PUBLISHABLE_KEY ? (
      <ClerkProvider publishableKey={CLERK_PUBLISHABLE_KEY}>
        <App />
      </ClerkProvider>
    ) : (
      <MissingClerkConfig />
    )}
  </React.StrictMode>
);

function MissingClerkConfig() {
  return (
    <main className="shell">
      <section className="panel missing-config">
        <h1>Clerk is not configured</h1>
        <p>Set <code>VITE_CLERK_PUBLISHABLE_KEY</code> in <code>web/.env</code> locally and in Vercel environment variables.</p>
      </section>
    </main>
  );
}
