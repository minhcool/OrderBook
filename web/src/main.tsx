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
  advanceSimulator,
  cancelOrder,
  fetchBook,
  fetchFills,
  fetchLeaderboard,
  fetchLobbies,
  fetchLobbyMembership,
  fetchMarketTrades,
  fetchMe,
  fetchOpenOrders,
  fetchPortfolio,
  fetchRoomMembership,
  fetchPrice,
  fetchRooms,
  fetchSymbols,
  health,
  joinRoom,
  joinLobby,
  leaveRoom,
  leaveLobby,
  replaceOrder,
  submitOrder
} from "./api";
import type {
  ActiveSession,
  BookSnapshot,
  FillRecord,
  GameLobby,
  GameRoom,
  LeaderboardRow,
  LobbyMembership,
  MarketScope,
  MarketPrice,
  MarketTradeRecord,
  MeSummary,
  OpenOrder,
  OrderMode,
  ParticipantTrack,
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
  const [leaderboard, setLeaderboard] = React.useState<LeaderboardRow[]>([]);
  const [marketTrades, setMarketTrades] = React.useState<MarketTradeRecord[]>([]);
  const [liveUpdates, setLiveUpdates] = React.useState(() => localStorage.getItem("orderbook.liveUpdates") !== "false");
  const liveRefreshInFlight = React.useRef(false);

  const [side, setSide] = React.useState<Side>("buy");
  const [mode, setMode] = React.useState<OrderMode>("limit");
  const [price, setPrice] = React.useState("100");
  const [quantity, setQuantity] = React.useState("5");

  const [replaceSide, setReplaceSide] = React.useState<Side>("buy");
  const [replaceOrderId, setReplaceOrderId] = React.useState("1001");
  const [replacePriceValue, setReplacePriceValue] = React.useState("101");
  const [replaceQuantity, setReplaceQuantity] = React.useState("5");
  const [cancelId, setCancelId] = React.useState("1001");
  const [joinTrack, setJoinTrack] = React.useState<ParticipantTrack>("manual");

  const selectedRoom = rooms.find((room) => room.id === roomId);
  const isCompetitive = selectedRoom?.mode === "competitive";
  const selectedLobby = (isCompetitive ? membership?.lobby : undefined) ?? lobbies.find((lobby) => lobby.id === lobbyId);
  const selectedRoomDetails = membership?.room ?? selectedRoom;
  const activeScope: MarketScope = {
    roomId,
    ...(isCompetitive ? { lobbyId } : {})
  };
  const isEntered = membership?.joined === true;
  const canViewMarket = Boolean(isSignedIn && selectedRoom && isEntered);
  const gameIsRunning = !isCompetitive || selectedLobby?.phase === "running";
  const canTrade = canViewMarket && gameIsRunning;
  const cooldownRemaining = membership?.cooldownRemainingSeconds ?? 0;
  const activeSession = membership?.activeSession ?? null;
  const activeSessionMatches = Boolean(activeSession && (
    isCompetitive
      ? activeSession.competitive && activeSession.id === lobbyId
      : !activeSession.competitive && activeSession.roomId === roomId
  ));
  const activeElsewhere = Boolean(activeSession && !activeSessionMatches);
  const entryDisabled = cooldownRemaining > 0
    || activeElsewhere
    || (isCompetitive && selectedLobby?.status !== "open");

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

      let nextMembership: LobbyMembership | null = null;
      const scope: MarketScope = {
        roomId: activeRoom.id,
        ...(activeLobby ? { lobbyId: activeLobby.id } : {})
      };

      if (isLoaded && isSignedIn) {
        const token = await getToken();
        if (token) {
          nextMembership = activeRoom.mode === "competitive" && activeLobby
            ? await fetchLobbyMembership(apiBase, activeLobby.id, token)
            : await fetchRoomMembership(apiBase, activeRoom.id, token);
        }
      }

      setRooms(nextRooms);
      setRoomId(activeRoom.id);
      setLobbies(nextLobbies);
      if (activeLobby) {
        setLobbyId(activeLobby.id);
      }
      setMembership(nextMembership);

      if (nextMembership?.joined) {
        const nextSymbols = await fetchSymbols(apiBase, scope);
        const activeSymbol = nextSymbols.includes(symbol) ? symbol : nextSymbols[0] ?? symbol;
        const [nextBook, nextMarketPrice, nextMarketTrades] = await Promise.all([
          fetchBook(apiBase, scope, activeSymbol, 10),
          fetchPrice(apiBase, scope, activeSymbol),
          fetchMarketTrades(apiBase, scope, activeSymbol, 20)
        ]);

        setSymbol(activeSymbol);
        setBook(nextBook);
        setSymbols(nextSymbols);
        setMarketPrice(nextMarketPrice);
        setMarketTrades(nextMarketTrades);
      } else {
        setBook({ symbol, bids: [], asks: [] });
        setSymbols([]);
        setMarketPrice(null);
        setMarketTrades([]);
        setMe(null);
        setOpenOrders([]);
        setFills([]);
        setPortfolio(null);
        setLeaderboard([]);
      }

      setApiOnline(true);
    } catch (error) {
      setApiOnline(false);
      pushActivity("API offline", error instanceof Error ? error.message : "Request failed", false);
    } finally {
      setLoading(false);
    }
  }, [apiBase, getToken, isLoaded, isSignedIn, lobbyId, pushActivity, roomId, symbol]);

  const refreshAccount = React.useCallback(async () => {
    if (!isLoaded || !isSignedIn) {
      setMembership(null);
      setMe(null);
      setOpenOrders([]);
      setFills([]);
      setPortfolio(null);
      setLeaderboard([]);
      setMarketTrades([]);
      return;
    }

    if (rooms.length === 0 || !selectedRoom || (isCompetitive && !lobbyId)) {
      return;
    }

    try {
      const token = await getToken();
      if (!token) {
        throw new Error("Could not read Clerk session token");
      }

      const nextMembership = isCompetitive
        ? await fetchLobbyMembership(apiBase, lobbyId, token)
        : await fetchRoomMembership(apiBase, roomId, token);
      setMembership(nextMembership);
      if (nextMembership.lobby) {
        setLobbies((items) => items.map((item) => item.id === nextMembership.lobby?.id ? nextMembership.lobby : item));
      }

      if (!nextMembership.joined) {
        setMe(null);
        setOpenOrders([]);
        setFills([]);
        setPortfolio(null);
        setLeaderboard([]);
        setMarketTrades([]);
        return;
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
      const nextLeaderboard = isCompetitive ? await fetchLeaderboard(apiBase, lobbyId, token) : [];

      setMe(nextMe);
      setOpenOrders(nextOpenOrders);
      setFills(nextFills);
      setPortfolio(nextPortfolio);
      setLeaderboard(nextLeaderboard);
    } catch (error) {
      pushActivity("Account refresh failed", error instanceof Error ? error.message : "Request failed", false);
    }
  }, [apiBase, getToken, isCompetitive, isLoaded, isSignedIn, lobbyId, pushActivity, roomId, rooms.length, selectedRoom]);

  const refreshAll = React.useCallback(async () => {
    await Promise.all([refresh(), refreshAccount()]);
  }, [refresh, refreshAccount]);

  React.useEffect(() => {
    localStorage.setItem("orderbook.liveUpdates", liveUpdates ? "true" : "false");
  }, [liveUpdates]);

  React.useEffect(() => {
    void refresh();
  }, [refresh]);

  React.useEffect(() => {
    void refreshAccount();
  }, [refreshAccount]);

  React.useEffect(() => {
    if (!liveUpdates || !isLoaded || !isSignedIn || !isEntered) {
      return;
    }

    const intervalId = window.setInterval(() => {
      if (liveRefreshInFlight.current) {
        return;
      }

      liveRefreshInFlight.current = true;
      void (async () => {
        try {
          const token = await getToken();
          if (token && !isCompetitive) {
            await advanceSimulator(apiBase, roomId, token, 1);
          }

          await refreshAll();
        } catch {
          setApiOnline(false);
        } finally {
          liveRefreshInFlight.current = false;
        }
      })();
    }, isCompetitive ? 2500 : 1500);

    return () => window.clearInterval(intervalId);
  }, [
    apiBase,
    getToken,
    isCompetitive,
    isEntered,
    isLoaded,
    isSignedIn,
    liveUpdates,
    refreshAll,
    roomId
  ]);

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

  async function handleEnterSession() {
    try {
      const token = await requireToken();
      const result = isCompetitive
        ? await joinLobby(apiBase, lobbyId, token, joinTrack)
        : await joinRoom(apiBase, roomId, token);
      setMembership({
        joined: result.joined,
        traderId: result.traderId,
        track: isCompetitive ? joinTrack : undefined,
        cooldownRemainingSeconds: result.cooldownRemainingSeconds,
        activeSession: result.activeSession,
        lobby: result.lobby,
        room: result.room
      });
      if (result.lobby) {
        setLobbies((items) => items.map((item) => item.id === result.lobby?.id ? result.lobby : item));
      }
      pushActivity(
        result.joined ? "Entered session" : "Enter failed",
        result.lobby
          ? `${result.lobby.name}: ${result.lobby.participantCount}/${result.lobby.capacity}`
          : result.message,
        result.joined
      );
      await refreshAll();
    } catch (error) {
      pushActivity("Enter failed", error instanceof Error ? error.message : "Request failed", false);
    }
  }

  async function handleExitSession() {
    try {
      const token = await requireToken();
      const result = isCompetitive
        ? await leaveLobby(apiBase, lobbyId, token)
        : await leaveRoom(apiBase, roomId, token);
      setMembership(null);
      if (result.lobby) {
        setLobbies((items) => items.map((item) => item.id === result.lobby?.id ? result.lobby : item));
      }
      setMe(null);
      setOpenOrders([]);
      setFills([]);
      setPortfolio(null);
      setLeaderboard([]);
      setSymbols([]);
      setBook({ symbol, bids: [], asks: [] });
      setMarketPrice(null);
      setMarketTrades([]);
      pushActivity(
        "Exited session",
        result.cooldownRemainingSeconds > 0
          ? `Resting orders canceled. Cooldown ${result.cooldownRemainingSeconds}s`
          : "Resting orders canceled",
        true
      );
      await refresh();
    } catch (error) {
      pushActivity("Exit failed", error instanceof Error ? error.message : "Request failed", false);
    }
  }

  function handleFocusActiveSession() {
    if (!activeSession) {
      return;
    }

    setRoomId(activeSession.roomId);
    if (activeSession.competitive) {
      setLobbyId(activeSession.id);
    }

    setMembership(null);
    setSymbols([]);
    setBook({ symbol, bids: [], asks: [] });
    setMarketPrice(null);
    setMarketTrades([]);
  }

  const spread = book.bids.length > 0 && book.asks.length > 0 ? book.asks[0].price - book.bids[0].price : null;
  const signedInName = user?.primaryEmailAddress?.emailAddress ?? user?.fullName ?? user?.id ?? "Signed in";
  const selectedAsset = selectedRoomDetails?.assets.find((asset) => asset.symbol === symbol);

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
          Symbol
          <input
            value={symbol}
            disabled={!canViewMarket}
            onChange={(event) => setSymbol(event.target.value.toUpperCase())}
          />
        </label>
        <button type="button" onClick={() => void refreshAll()}>
          <PlugZap size={17} />
          Connect
        </button>
        <label className="toggle-row">
          <input
            type="checkbox"
            checked={liveUpdates}
            onChange={(event) => setLiveUpdates(event.target.checked)}
          />
          Live
        </label>
        <div className="symbols">
          {canViewMarket
            ? symbols.map((item) => (
                <button key={item} type="button" className={item === symbol ? "selected" : ""} onClick={() => setSymbol(item)}>
                  {item}
                </button>
              ))
            : null}
        </div>
      </section>

      <SessionPanel
        rooms={rooms}
        lobbies={lobbies}
        selectedRoom={selectedRoom}
        selectedLobby={selectedLobby}
        selectedAsset={selectedAsset}
        roomId={roomId}
        lobbyId={lobbyId}
        isSignedIn={Boolean(isSignedIn)}
        isEntered={isEntered}
        isCompetitive={Boolean(isCompetitive)}
        gameIsRunning={gameIsRunning}
        cooldownRemaining={cooldownRemaining}
        entryDisabled={entryDisabled}
        activeSession={activeSession}
        activeElsewhere={activeElsewhere}
        joinTrack={joinTrack}
        currentTrack={membership?.track}
        onJoinTrackChange={setJoinTrack}
        onSelectRoom={(nextRoomId) => {
          setRoomId(nextRoomId);
          setMembership(null);
          setSymbols([]);
          setBook({ symbol, bids: [], asks: [] });
          setMarketPrice(null);
          setMarketTrades([]);
        }}
        onSelectLobby={(nextLobbyId) => {
          setLobbyId(nextLobbyId);
          setMembership(null);
          setSymbols([]);
          setBook({ symbol, bids: [], asks: [] });
          setMarketPrice(null);
          setMarketTrades([]);
        }}
        onEnter={() => void handleEnterSession()}
        onExit={() => void handleExitSession()}
        onFocusActive={handleFocusActiveSession}
      />

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
          {!canViewMarket ? (
            <div className="locked-state">Enter the selected room or lobby to view market data.</div>
          ) : (
            <div className="book-grid">
              <BookSide title="Bids" levels={book.bids} tone="bid" />
              <BookSide title="Asks" levels={book.asks} tone="ask" />
            </div>
          )}
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
            {isSignedIn && !isEntered ? (
              <div className="auth-callout">
                <strong>{isCompetitive ? "Join this lobby" : "Enter this room"}</strong>
                <span>Market data and orders unlock after the API confirms entry.</span>
              </div>
            ) : null}
            {isSignedIn && isEntered && isCompetitive && !gameIsRunning ? (
              <div className="auth-callout">
                <strong>{selectedLobby?.phase === "finished" ? "Game finished" : "Waiting for start"}</strong>
                <span>
                  {selectedLobby?.phase === "starting"
                    ? `Trading opens in ${selectedLobby.startsInSeconds}s.`
                    : "Trading opens when the lobby reaches the start condition."}
                </span>
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

        <TradeTapePanel trades={marketTrades} isEntered={canViewMarket} />

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
          leaderboard={leaderboard}
          track={membership?.track}
          isSignedIn={Boolean(isSignedIn)}
          isEntered={isEntered}
          onRefresh={() => void refreshAccount()}
        />
      </div>
    </main>
  );
}

function SessionPanel({
  rooms,
  lobbies,
  selectedRoom,
  selectedLobby,
  selectedAsset,
  roomId,
  lobbyId,
  isSignedIn,
  isEntered,
  isCompetitive,
  gameIsRunning,
  cooldownRemaining,
  entryDisabled,
  activeSession,
  activeElsewhere,
  joinTrack,
  currentTrack,
  onJoinTrackChange,
  onSelectRoom,
  onSelectLobby,
  onEnter,
  onExit,
  onFocusActive
}: {
  rooms: GameRoom[];
  lobbies: GameLobby[];
  selectedRoom?: GameRoom;
  selectedLobby?: GameLobby;
  selectedAsset?: GameRoom["assets"][number];
  roomId: string;
  lobbyId: string;
  isSignedIn: boolean;
  isEntered: boolean;
  isCompetitive: boolean;
  gameIsRunning: boolean;
  cooldownRemaining: number;
  entryDisabled: boolean;
  activeSession: ActiveSession | null;
  activeElsewhere: boolean;
  joinTrack: ParticipantTrack;
  currentTrack?: ParticipantTrack;
  onJoinTrackChange: (track: ParticipantTrack) => void;
  onSelectRoom: (roomId: string) => void;
  onSelectLobby: (lobbyId: string) => void;
  onEnter: () => void;
  onExit: () => void;
  onFocusActive: () => void;
}) {
  const phaseLabel = isCompetitive
    ? selectedLobby?.phase ?? "waiting"
    : "live";
  const timerLabel = !selectedLobby
    ? `${selectedRoom?.gameDurationSeconds ?? 0}s`
    : selectedLobby.phase === "starting"
      ? `starts ${selectedLobby.startsInSeconds}s`
      : selectedLobby.phase === "running"
        ? `ends ${selectedLobby.endsInSeconds}s`
        : `${selectedLobby.gameDurationSeconds}s`;

  return (
    <section className="panel session-panel">
      <div className="panel-title">
        <Server size={18} />
        <h2>Session</h2>
      </div>

      <div className="session-layout">
        <div className="session-column">
          <div className="section-heading">Rooms</div>
          <div className="session-list">
            {rooms.length === 0 ? (
              <div className="empty-state">No rooms</div>
            ) : (
              rooms.map((room) => (
                <button
                  key={room.id}
                  type="button"
                  className={`session-row ${room.id === roomId ? "selected" : ""}`}
                  onClick={() => onSelectRoom(room.id)}
                >
                  <span className="row-main">
                    <strong>{room.name}</strong>
                    <span>{room.mode} / {room.difficulty}</span>
                  </span>
                  <span className="row-meta">
                    <span>{formatNumber(room.startingCash)}</span>
                    <span>{room.gameDurationSeconds}s</span>
                  </span>
                </button>
              ))
            )}
          </div>
        </div>

        <div className="session-column">
          <div className="section-heading">{isCompetitive ? "Lobbies" : "Access"}</div>
          {isCompetitive ? (
            <div className="session-list">
              {lobbies.map((lobby) => {
                const filledPercent = Math.min(100, (lobby.participantCount / Math.max(1, lobby.capacity)) * 100);
                return (
                  <button
                    key={lobby.id}
                    type="button"
                    className={`session-row lobby-row ${lobby.id === lobbyId ? "selected" : ""}`}
                    onClick={() => onSelectLobby(lobby.id)}
                  >
                    <span className="row-main">
                      <strong>{lobby.name}</strong>
                      <span>{lobby.participantCount}/{lobby.capacity} players / {lobby.phase}</span>
                      <span className="capacity-bar"><span style={{ width: `${filledPercent}%` }} /></span>
                    </span>
                    <span className="row-meta">
                      <span>{lobby.status}</span>
                      <span>{lobby.phase === "starting" ? `${lobby.startsInSeconds}s` : `${lobby.spotsRemaining} left`}</span>
                    </span>
                  </button>
                );
              })}
            </div>
          ) : (
            <div className="session-list">
              <div className="session-row static-row">
                <span className="row-main">
                  <strong>Personal session</strong>
                  <span>{selectedRoom?.houseLiquidity ? "simulated liquidity" : "manual liquidity"}</span>
                </span>
                <span className="row-meta">
                  <span>1 seat</span>
                  <span>{selectedRoom?.gameDurationSeconds ?? 0}s</span>
                </span>
              </div>
            </div>
          )}
        </div>
      </div>

      <div className="session-action-row">
        <div className="state-pills">
          <span>{selectedRoom?.mode ?? "single"}</span>
          <span>{phaseLabel}</span>
          <span>{timerLabel}</span>
          <span>cash {selectedRoom ? formatNumber(selectedRoom.startingCash) : "-"}</span>
          <span>{selectedAsset?.behavior ?? "locked"}</span>
          <span>{selectedAsset?.source ?? "locked"}</span>
          {activeSession ? <span>active {activeSession.competitive ? activeSession.id : activeSession.roomId}</span> : null}
          {isEntered && isCompetitive ? <span>{currentTrack ?? joinTrack}</span> : null}
          {isEntered && !gameIsRunning ? <span>orders locked</span> : null}
        </div>

        {isCompetitive && !isEntered ? (
          <div className="join-track">
            <span>Track</span>
            <Segmented<ParticipantTrack> value={joinTrack} onChange={onJoinTrackChange} options={[
              { label: "Manual", value: "manual" },
              { label: "Bot", value: "bot" }
            ]} />
          </div>
        ) : null}

        {activeElsewhere ? (
          <button type="button" onClick={onFocusActive}>
            <PlugZap size={16} />
            Focus active
          </button>
        ) : null}

        {isSignedIn && selectedRoom ? (
          isEntered ? (
            <button type="button" className="lobby-action leave" onClick={onExit}>
              <LogOut size={16} />
              Exit
            </button>
          ) : (
            <button
              type="button"
              className="lobby-action join"
              disabled={entryDisabled}
              onClick={onEnter}
            >
              <UserPlus size={16} />
              {cooldownRemaining > 0
                ? `Wait ${cooldownRemaining}s`
                : activeElsewhere
                  ? "Active elsewhere"
                  : isCompetitive
                    ? selectedLobby?.status !== "open"
                      ? selectedLobby?.status === "full" ? "Lobby full" : "Lobby locked"
                      : "Join lobby"
                    : "Enter room"}
            </button>
          )
        ) : null}
      </div>
    </section>
  );
}

function TradeTapePanel({ trades, isEntered }: { trades: MarketTradeRecord[]; isEntered: boolean }) {
  return (
    <section className="panel tape-panel">
      <div className="panel-title">
        <Activity size={18} />
        <h2>Trades</h2>
      </div>

      {!isEntered ? (
        <div className="locked-state compact">Enter the selected room or lobby to view trades.</div>
      ) : trades.length === 0 ? (
        <div className="empty-state">No trades</div>
      ) : (
        <div className="trade-list">
          {trades.slice(0, 16).map((trade) => (
            <div className="trade-row" key={trade.sequence}>
              <div className="data-main">
                <strong>
                  #{trade.sequence} <span className={`side-chip ${trade.takerSide}`}>{formatSide(trade.takerSide)}</span>
                </strong>
                <span>Taker {formatNumber(trade.takerTraderId)} / maker {formatNumber(trade.makerTraderId)}</span>
              </div>
              <div className="data-values">
                <strong>{formatNumber(trade.quantity)} @ {formatNumber(trade.price)}</strong>
                <span>{formatNumber(trade.notional)}</span>
              </div>
            </div>
          ))}
        </div>
      )}
    </section>
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
  leaderboard,
  track,
  isSignedIn,
  isEntered,
  onRefresh
}: {
  me: MeSummary | null;
  openOrders: OpenOrder[];
  fills: FillRecord[];
  portfolio: PortfolioRecord | null;
  leaderboard: LeaderboardRow[];
  track?: ParticipantTrack;
  isSignedIn: boolean;
  isEntered: boolean;
  onRefresh: () => void;
}) {
  const positions = portfolio?.positions ?? [];

  return (
    <section className="panel account-panel">
      <div className="panel-title">
        <Activity size={18} />
        <h2>Account</h2>
        <button className="icon-button ghost" type="button" onClick={onRefresh} title="Refresh account" disabled={!isSignedIn || !isEntered}>
          <RefreshCw size={17} />
        </button>
      </div>

      {!isSignedIn ? (
        <div className="auth-callout">
          <strong>Sign in required</strong>
          <span>Your orders, fills, and positions are keyed by your Clerk user.</span>
        </div>
      ) : !isEntered ? (
        <div className="auth-callout">
          <strong>Enter a session</strong>
          <span>Cash, positions, orders, and fills are scoped to the active room or lobby.</span>
        </div>
      ) : (
        <>
          <div className="account-summary">
            <div className="account-stat">
              <span>Trader ID</span>
              <strong>{me ? formatNumber(me.traderId) : "-"}</strong>
            </div>
            <div className="account-stat">
              <span>Cash</span>
              <strong>{portfolio ? formatSignedNumber(portfolio.cash) : "-"}</strong>
            </div>
            <div className="account-stat">
              <span>Available</span>
              <strong>{portfolio ? formatSignedNumber(portfolio.availableCash) : "-"}</strong>
            </div>
            <div className="account-stat">
              <span>Reserved</span>
              <strong>{portfolio ? formatSignedNumber(portfolio.reservedCash) : "-"}</strong>
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
            {track ? (
              <div className="account-stat">
                <span>Track</span>
                <strong>{track}</strong>
              </div>
            ) : null}
            <div className="account-stat">
              <span>Trading PnL</span>
              <strong className={portfolio && portfolio.tradingPnl < 0 ? "sell-text" : "buy-text"}>
                {portfolio ? formatSignedNumber(portfolio.tradingPnl) : "-"}
              </strong>
            </div>
            <div className="account-stat">
              <span>Unrealized</span>
              <strong className={portfolio && portfolio.unrealizedPnl < 0 ? "sell-text" : "buy-text"}>
                {portfolio ? formatSignedNumber(portfolio.unrealizedPnl) : "-"}
              </strong>
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

            {leaderboard.length > 0 ? (
              <AccountSection title="Leaderboard">
                {leaderboard.map((row) => (
                  <div className="data-row" key={`${row.track}-${row.traderId}`}>
                    <div className="data-main">
                      <strong>
                        #{row.rank} <span className="side-chip neutral">{row.track}</span>
                      </strong>
                      <span>Trader {formatNumber(row.traderId)}</span>
                    </div>
                    <div className="data-values">
                      <strong>{formatSignedNumber(row.pnl)}</strong>
                      <span>Elo {row.ratingBefore} {"->"} {row.ratingAfter}</span>
                    </div>
                  </div>
                ))}
              </AccountSection>
            ) : null}
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
