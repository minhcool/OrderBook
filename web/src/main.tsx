import React from "react";
import ReactDOM from "react-dom/client";
import { ClerkProvider, Show, SignInButton, SignUpButton, UserButton, useAuth, useUser } from "@clerk/react";
import {
  Activity,
  CircleOff,
  Eraser,
  LogOut,
  PlugZap,
  RefreshCw,
  Replace,
  Send,
  Server,
  UserPlus,
  Wifi,
  WifiOff
} from "lucide-react";

import {
  cancelOrder,
  fetchBook,
  fetchFills,
  fetchLobbies,
  fetchLobbyMembership,
  fetchMe,
  fetchOpenOrders,
  fetchPortfolio,
  fetchPrice,
  fetchRooms,
  fetchSymbols,
  health,
  joinLobby,
  leaveLobby,
  replaceOrder,
  submitOrder
} from "./api";
import type {
  BookSnapshot,
  FillRecord,
  GameLobby,
  GameRoom,
  LobbyMembership,
  MarketScope,
  MarketPrice,
  MeSummary,
  OpenOrder,
  OrderMode,
  PortfolioRecord,
  Side,
  SubmitResult
} from "./types";
import "./styles.css";

const DEFAULT_API_BASE = import.meta.env.VITE_ORDERBOOK_API_URL ?? "http://localhost:8080";
const CLERK_PUBLISHABLE_KEY = import.meta.env.VITE_CLERK_PUBLISHABLE_KEY;
const DEFAULT_ROOM_ID = "solo-alpha";
const DEFAULT_LOBBY_ID = "aurora-open-10";
const DEFAULT_SYMBOL = "NOVA";

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

function formatSignedNumber(value: number): string {
  if (!Number.isFinite(value) || value === 0) {
    return "0";
  }

  return `${value > 0 ? "+" : "-"}${formatNumber(Math.abs(value))}`;
}

function formatSide(side: Side): string {
  return side === "buy" ? "Buy" : "Sell";
}

function resultSummary(result: SubmitResult): string {
  const avg = result.filledQuantity > 0 ? ` avg ${result.averagePrice.toFixed(2)}` : "";
  return `${result.message}: filled ${result.filledQuantity}, resting ${result.restingQuantity}${avg}`;
}

function App() {
  const { getToken, isLoaded, isSignedIn } = useAuth();
  const { user } = useUser();

  const [apiBase, setApiBase] = React.useState(() => localStorage.getItem("orderbook.apiBase") ?? DEFAULT_API_BASE);
  const [roomId, setRoomId] = React.useState(() => localStorage.getItem("orderbook.roomId") ?? DEFAULT_ROOM_ID);
  const [rooms, setRooms] = React.useState<GameRoom[]>([]);
  const [lobbyId, setLobbyId] = React.useState(() => localStorage.getItem("orderbook.lobbyId") ?? DEFAULT_LOBBY_ID);
  const [lobbies, setLobbies] = React.useState<GameLobby[]>([]);
  const [membership, setMembership] = React.useState<LobbyMembership | null>(null);
  const [symbol, setSymbol] = React.useState(DEFAULT_SYMBOL);
  const [symbols, setSymbols] = React.useState<string[]>([]);
  const [book, setBook] = React.useState<BookSnapshot>({ symbol: DEFAULT_SYMBOL, bids: [], asks: [] });
  const [marketPrice, setMarketPrice] = React.useState<MarketPrice | null>(null);
  const [apiOnline, setApiOnline] = React.useState(false);
  const [loading, setLoading] = React.useState(false);
  const [activity, setActivity] = React.useState<ActivityItem[]>([]);
  const [me, setMe] = React.useState<MeSummary | null>(null);
  const [openOrders, setOpenOrders] = React.useState<OpenOrder[]>([]);
  const [fills, setFills] = React.useState<FillRecord[]>([]);
  const [portfolio, setPortfolio] = React.useState<PortfolioRecord | null>(null);

  const [side, setSide] = React.useState<Side>("buy");
  const [mode, setMode] = React.useState<OrderMode>("limit");
  const [price, setPrice] = React.useState("100");
  const [quantity, setQuantity] = React.useState("5");

  const [replaceSide, setReplaceSide] = React.useState<Side>("buy");
  const [replaceOrderId, setReplaceOrderId] = React.useState("1001");
  const [replacePriceValue, setReplacePriceValue] = React.useState("101");
  const [replaceQuantity, setReplaceQuantity] = React.useState("5");
  const [cancelId, setCancelId] = React.useState("1001");

  const selectedRoom = rooms.find((room) => room.id === roomId);
  const selectedLobby = lobbies.find((lobby) => lobby.id === lobbyId);
  const isCompetitive = selectedRoom?.mode === "competitive";
  const activeScope: MarketScope = {
    roomId,
    ...(isCompetitive ? { lobbyId } : {})
  };
  const canTrade = Boolean(isSignedIn && selectedRoom) && (!isCompetitive || membership?.joined === true);

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
    localStorage.setItem("orderbook.roomId", roomId);
    localStorage.setItem("orderbook.lobbyId", lobbyId);

    try {
      await health(apiBase);
      const nextRooms = await fetchRooms(apiBase);
      const activeRoom = nextRooms.find((room) => room.id === roomId) ?? nextRooms[0];
      if (!activeRoom) {
        throw new Error("API returned no game rooms");
      }

      const nextLobbies = activeRoom.mode === "competitive"
        ? await fetchLobbies(apiBase, activeRoom.id)
        : [];
      const activeLobby = activeRoom.mode === "competitive"
        ? nextLobbies.find((lobby) => lobby.id === lobbyId) ?? nextLobbies[0]
        : undefined;
      if (activeRoom.mode === "competitive" && !activeLobby) {
        throw new Error("This competitive room has no available lobbies");
      }

      const scope: MarketScope = {
        roomId: activeRoom.id,
        ...(activeLobby ? { lobbyId: activeLobby.id } : {})
      };
      const nextSymbols = await fetchSymbols(apiBase, scope);
      const activeSymbol = nextSymbols.includes(symbol) ? symbol : nextSymbols[0] ?? symbol;
      const [nextBook, nextMarketPrice] = await Promise.all([
        fetchBook(apiBase, scope, activeSymbol, 10),
        fetchPrice(apiBase, scope, activeSymbol)
      ]);

      setRooms(nextRooms);
      setRoomId(activeRoom.id);
      setLobbies(nextLobbies);
      if (activeLobby) {
        setLobbyId(activeLobby.id);
      }
      setSymbol(activeSymbol);
      setBook(nextBook);
      setSymbols(nextSymbols);
      setMarketPrice(nextMarketPrice);
      setApiOnline(true);
    } catch (error) {
      setApiOnline(false);
      pushActivity("API offline", error instanceof Error ? error.message : "Request failed", false);
    } finally {
      setLoading(false);
    }
  }, [apiBase, lobbyId, pushActivity, roomId, symbol]);

  const refreshAccount = React.useCallback(async () => {
    if (!isLoaded || !isSignedIn) {
      setMembership(null);
      setMe(null);
      setOpenOrders([]);
      setFills([]);
      setPortfolio(null);
      return;
    }

    if (rooms.length === 0 || (isCompetitive && !lobbyId)) {
      return;
    }

    try {
      const token = await getToken();
      if (!token) {
        throw new Error("Could not read Clerk session token");
      }

      if (isCompetitive) {
        const nextMembership = await fetchLobbyMembership(apiBase, lobbyId, token);
        setMembership(nextMembership);
        if (!nextMembership.joined) {
          setMe(null);
          setOpenOrders([]);
          setFills([]);
          setPortfolio(null);
          return;
        }
      } else {
        setMembership(null);
      }

      const scope: MarketScope = {
        roomId,
        ...(isCompetitive ? { lobbyId } : {})
      };
      const [nextMe, nextOpenOrders, nextFills, nextPortfolio] = await Promise.all([
        fetchMe(apiBase, scope, token),
        fetchOpenOrders(apiBase, scope, token),
        fetchFills(apiBase, scope, token),
        fetchPortfolio(apiBase, scope, token)
      ]);

      setMe(nextMe);
      setOpenOrders(nextOpenOrders);
      setFills(nextFills);
      setPortfolio(nextPortfolio);
    } catch (error) {
      pushActivity("Account refresh failed", error instanceof Error ? error.message : "Request failed", false);
    }
  }, [apiBase, getToken, isCompetitive, isLoaded, isSignedIn, lobbyId, pushActivity, roomId, rooms.length]);

  const refreshAll = React.useCallback(async () => {
    await Promise.all([refresh(), refreshAccount()]);
  }, [refresh, refreshAccount]);

  React.useEffect(() => {
    void refresh();
  }, [refresh]);

  React.useEffect(() => {
    void refreshAccount();
  }, [refreshAccount]);

  async function handleSubmitOrder(event: React.FormEvent) {
    event.preventDefault();

    const payload = {
      symbol,
      quantity: toNumber(quantity),
      ...(mode === "market" ? {} : { price: toNumber(price) })
    };

    try {
      const token = await requireToken();
      const result = await submitOrder(apiBase, activeScope, token, side, mode, payload);
      pushActivity(`${mode.toUpperCase()} ${side.toUpperCase()} #${result.orderId}`, resultSummary(result), result.accepted);
      if (result.accepted && result.restingQuantity > 0) {
        setReplaceSide(side);
        setReplaceOrderId(String(result.orderId));
        setCancelId(String(result.orderId));
      }
      await refreshAll();
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
      const result = await replaceOrder(apiBase, activeScope, token, replaceSide, payload);
      pushActivity(`REPLACE ${replaceSide.toUpperCase()} #${payload.orderId}`, resultSummary(result), result.accepted);
      await refreshAll();
    } catch (error) {
      pushActivity("Replace failed", error instanceof Error ? error.message : "Request failed", false);
    }
  }

  async function handleCancel(event: React.FormEvent) {
    event.preventDefault();

    try {
      const token = await requireToken();
      const result = await cancelOrder(apiBase, activeScope, token, symbol, toNumber(cancelId));
      pushActivity(`CANCEL #${cancelId}`, result.canceled ? "canceled" : "order not found", result.canceled);
      await refreshAll();
    } catch (error) {
      pushActivity("Cancel failed", error instanceof Error ? error.message : "Request failed", false);
    }
  }

  async function handleJoinLobby() {
    try {
      const token = await requireToken();
      const result = await joinLobby(apiBase, lobbyId, token);
      setMembership({ joined: result.joined, traderId: 0 });
      setLobbies((items) => items.map((item) => item.id === result.lobby.id ? result.lobby : item));
      pushActivity("Lobby joined", `${result.lobby.name}: ${result.lobby.participantCount}/${result.lobby.capacity}`, true);
      await refreshAccount();
    } catch (error) {
      pushActivity("Join failed", error instanceof Error ? error.message : "Request failed", false);
    }
  }

  async function handleLeaveLobby() {
    try {
      const token = await requireToken();
      const result = await leaveLobby(apiBase, lobbyId, token);
      setMembership(null);
      setLobbies((items) => items.map((item) => item.id === result.lobby.id ? result.lobby : item));
      setMe(null);
      setOpenOrders([]);
      setFills([]);
      setPortfolio(null);
      pushActivity("Lobby left", "Your resting orders in this lobby were canceled", true);
      await refresh();
    } catch (error) {
      pushActivity("Leave failed", error instanceof Error ? error.message : "Request failed", false);
    }
  }

  const spread = book.bids.length > 0 && book.asks.length > 0 ? book.asks[0].price - book.bids[0].price : null;
  const signedInName = user?.primaryEmailAddress?.emailAddress ?? user?.fullName ?? user?.id ?? "Signed in";
  const selectedAsset = selectedRoom?.assets.find((asset) => asset.symbol === symbol);

  return (
    <main className="shell">
      <header className="topbar">
        <div>
          <h1>Trading Game Console</h1>
          <p>{selectedRoom ? `${selectedRoom.name} / ${symbol}` : symbol}</p>
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
          <button className="icon-button" onClick={() => void refreshAll()} title="Refresh" type="button">
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
          Room
          <select
            value={roomId}
            onChange={(event) => {
              setRoomId(event.target.value);
              setMembership(null);
            }}
          >
            {rooms.length === 0 ? (
              <option value={roomId}>{roomId}</option>
            ) : (
              rooms.map((room) => (
                <option key={room.id} value={room.id}>
                  {room.name}
                </option>
              ))
            )}
          </select>
        </label>
        <label>
          Lobby
          <select
            value={isCompetitive ? lobbyId : ""}
            disabled={!isCompetitive}
            onChange={(event) => {
              setLobbyId(event.target.value);
              setMembership(null);
            }}
          >
            {!isCompetitive ? (
              <option value="">Personal session</option>
            ) : (
              lobbies.map((lobby) => (
                <option key={lobby.id} value={lobby.id}>
                  {lobby.name} ({lobby.participantCount}/{lobby.capacity})
                </option>
              ))
            )}
          </select>
        </label>
        <label>
          Symbol
          <input value={symbol} onChange={(event) => setSymbol(event.target.value.toUpperCase())} />
        </label>
        <button type="button" onClick={() => void refreshAll()}>
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
        <div className="room-meta">
          <span>{selectedRoom?.mode ?? "single"}</span>
          <span>{selectedRoom?.difficulty ?? "dev"}</span>
          <span>{selectedAsset?.behavior ?? "manual"}</span>
          <span>{selectedAsset?.source ?? "sandbox"}</span>
          {selectedLobby ? (
            <>
              <span>{selectedLobby.participantCount}/{selectedLobby.capacity} players</span>
              <span>{selectedLobby.status}</span>
            </>
          ) : null}
          {isCompetitive && isSignedIn ? (
            membership?.joined ? (
              <button type="button" className="lobby-action leave" onClick={() => void handleLeaveLobby()}>
                <LogOut size={16} />
                Leave lobby
              </button>
            ) : (
              <button
                type="button"
                className="lobby-action join"
                disabled={selectedLobby?.status === "full"}
                onClick={() => void handleJoinLobby()}
              >
                <UserPlus size={16} />
                {selectedLobby?.status === "full" ? "Lobby full" : "Join lobby"}
              </button>
            )
          ) : null}
        </div>
      </section>

      <section className="market-strip">
        <Metric label="Best bid" value={book.bids[0] ? formatNumber(book.bids[0].price) : "-"} />
        <Metric label="Best ask" value={book.asks[0] ? formatNumber(book.asks[0].price) : "-"} />
        <Metric label="Spread" value={spread === null ? "-" : formatNumber(spread)} />
        <Metric label="Last trade" value={marketPrice?.hasPrice ? formatNumber(marketPrice.lastPrice) : "-"} />
        <Metric label="VWAP 5" value={marketPrice?.hasPrice ? formatNumber(marketPrice.averagePrice5) : "-"} />
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
            {isSignedIn && isCompetitive && !membership?.joined ? (
              <div className="auth-callout">
                <strong>Join this lobby to trade</strong>
                <span>You can inspect its market first. Capacity is enforced by the API.</span>
              </div>
            ) : null}
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
              Price
              <input inputMode="numeric" value={price} disabled={mode === "market"} onChange={(event) => setPrice(event.target.value)} />
            </label>
            <label>
              Quantity
              <input inputMode="numeric" value={quantity} onChange={(event) => setQuantity(event.target.value)} />
            </label>
            <button className={`primary ${side}`} type="submit" disabled={!canTrade}>
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
            <button type="submit" disabled={!canTrade}>
              <Replace size={17} />
              Replace
            </button>
          </form>

          <form className="cancel-row" onSubmit={handleCancel}>
            <label>
              Cancel ID
              <input inputMode="numeric" value={cancelId} onChange={(event) => setCancelId(event.target.value)} />
            </label>
            <button type="submit" className="danger" disabled={!canTrade}>
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

        <AccountPanel
          me={me}
          openOrders={openOrders}
          fills={fills}
          portfolio={portfolio}
          isSignedIn={Boolean(isSignedIn)}
          onRefresh={() => void refreshAccount()}
        />
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

function AccountPanel({
  me,
  openOrders,
  fills,
  portfolio,
  isSignedIn,
  onRefresh
}: {
  me: MeSummary | null;
  openOrders: OpenOrder[];
  fills: FillRecord[];
  portfolio: PortfolioRecord | null;
  isSignedIn: boolean;
  onRefresh: () => void;
}) {
  const positions = portfolio?.positions ?? [];

  return (
    <section className="panel account-panel">
      <div className="panel-title">
        <Activity size={18} />
        <h2>Account</h2>
        <button className="icon-button ghost" type="button" onClick={onRefresh} title="Refresh account" disabled={!isSignedIn}>
          <RefreshCw size={17} />
        </button>
      </div>

      {!isSignedIn ? (
        <div className="auth-callout">
          <strong>Sign in required</strong>
          <span>Your orders, fills, and positions are keyed by your Clerk user.</span>
        </div>
      ) : (
        <>
          <div className="account-summary">
            <div className="account-stat">
              <span>Trader ID</span>
              <strong>{me ? formatNumber(me.traderId) : "-"}</strong>
            </div>
            <div className="account-stat">
              <span>Cash Flow</span>
              <strong>{portfolio ? formatSignedNumber(portfolio.cashFlow) : "-"}</strong>
            </div>
            <div className="account-stat">
              <span>Market Value</span>
              <strong>{portfolio ? formatSignedNumber(portfolio.marketValue) : "-"}</strong>
            </div>
            <div className="account-stat">
              <span>Est. Value</span>
              <strong className={portfolio && portfolio.estimatedValue < 0 ? "sell-text" : "buy-text"}>
                {portfolio ? formatSignedNumber(portfolio.estimatedValue) : "-"}
              </strong>
            </div>
            <div className="account-stat">
              <span>Open Orders</span>
              <strong>{me ? formatNumber(me.openOrderCount) : "-"}</strong>
            </div>
            <div className="account-stat">
              <span>Fills</span>
              <strong>{me ? formatNumber(me.fillCount) : "-"}</strong>
            </div>
            <div className="account-stat">
              <span>Positions</span>
              <strong>{me ? formatNumber(me.positionCount) : "-"}</strong>
            </div>
          </div>

          <div className="account-grid">
            <AccountSection title="Positions">
              {positions.length === 0 ? (
                <div className="empty-state">No positions</div>
              ) : (
                positions.map((position) => (
                  <div className="data-row" key={position.symbol}>
                    <div className="data-main">
                      <strong>{position.symbol}</strong>
                      <span>
                        Avg {position.averageEntryPrice ? formatNumber(position.averageEntryPrice) : "-"} / mark{" "}
                        {position.hasMark ? formatNumber(position.markPrice) : "-"}
                      </span>
                    </div>
                    <div className="data-values">
                      <strong className={position.quantity >= 0 ? "buy-text" : "sell-text"}>
                        {formatSignedNumber(position.quantity)}
                      </strong>
                      <span className={position.unrealizedPnl >= 0 ? "buy-text" : "sell-text"}>
                        PnL {position.hasMark ? formatSignedNumber(position.unrealizedPnl) : "-"}
                      </span>
                    </div>
                  </div>
                ))
              )}
            </AccountSection>

            <AccountSection title="Open Orders">
              {openOrders.length === 0 ? (
                <div className="empty-state">No open orders</div>
              ) : (
                openOrders.map((order) => (
                  <div className="data-row" key={`${order.symbol}-${order.orderId}`}>
                    <div className="data-main">
                      <strong>
                        #{order.orderId} <span className={`side-chip ${order.side}`}>{formatSide(order.side)}</span>
                      </strong>
                      <span>{order.symbol} @ {formatNumber(order.price)}</span>
                    </div>
                    <div className="data-values">
                      <strong>{formatNumber(order.remainingQuantity)}</strong>
                      <span>of {formatNumber(order.quantity)}</span>
                    </div>
                  </div>
                ))
              )}
            </AccountSection>

            <AccountSection title="Recent Fills">
              {fills.length === 0 ? (
                <div className="empty-state">No fills</div>
              ) : (
                fills.slice(0, 12).map((fill) => (
                  <div className="data-row" key={fill.sequence}>
                    <div className="data-main">
                      <strong>
                        #{fill.orderId} <span className={`side-chip ${fill.side}`}>{formatSide(fill.side)}</span>
                      </strong>
                      <span>{fill.symbol} {fill.liquidity} vs #{fill.counterpartyOrderId}</span>
                    </div>
                    <div className="data-values">
                      <strong>{formatNumber(fill.quantity)} @ {formatNumber(fill.price)}</strong>
                      <span>Notional {formatNumber(fill.notional)}</span>
                    </div>
                  </div>
                ))
              )}
            </AccountSection>
          </div>
        </>
      )}
    </section>
  );
}

function AccountSection({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="account-section">
      <h3>{title}</h3>
      <div className="data-list">{children}</div>
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
