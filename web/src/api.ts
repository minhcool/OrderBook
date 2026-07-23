import type {
  BookSnapshot,
  FillRecord,
  GameLobby,
  GameRoom,
  JoinResult,
  LeaderboardRow,
  LeaveResult,
  LobbyMembership,
  MarketScope,
  MarketPrice,
  MarketTradeRecord,
  MeSummary,
  NewOrderRequest,
  OpenOrder,
  OrderMode,
  PortfolioRecord,
  PositionRecord,
  ReplaceOrderRequest,
  SessionSummary,
  Side,
  SubmitResult
} from "./types";

export class ApiError extends Error {
  constructor(message: string, public readonly status?: number) {
    super(message);
  }
}

async function requestJson<T>(url: string, init?: RequestInit): Promise<T> {
  const response = await fetch(url, init);
  const text = await response.text();
  const data = text.length > 0 ? JSON.parse(text) : {};

  if (!response.ok) {
    throw new ApiError(data.error ?? response.statusText, response.status);
  }

  if (data.error) {
    throw new ApiError(data.error, response.status);
  }

  return data as T;
}

function authHeaders(token: string): HeadersInit {
  return {
    "Content-Type": "application/json",
    Authorization: `Bearer ${token}`
  };
}

export async function health(apiBase: string): Promise<{ ok: boolean }> {
  return requestJson(`${apiBase}/health`);
}

function scopedBase(apiBase: string, scope?: MarketScope): string {
  if (scope?.lobbyId) {
    return `${apiBase}/lobbies/${encodeURIComponent(scope.lobbyId)}`;
  }

  return scope?.roomId ? `${apiBase}/rooms/${encodeURIComponent(scope.roomId)}` : apiBase;
}

export async function fetchRooms(apiBase: string): Promise<GameRoom[]> {
  const data = await requestJson<{ rooms: GameRoom[] }>(`${apiBase}/rooms`);
  return data.rooms;
}

export async function fetchLobbies(apiBase: string, roomId?: string): Promise<GameLobby[]> {
  const endpoint = roomId
    ? `${apiBase}/rooms/${encodeURIComponent(roomId)}/lobbies`
    : `${apiBase}/lobbies`;
  const data = await requestJson<{ lobbies: GameLobby[] }>(endpoint);
  return data.lobbies;
}

export async function fetchActiveSession(apiBase: string, token: string): Promise<SessionSummary> {
  return requestJson(`${apiBase}/me/session`, {
    headers: authHeaders(token)
  });
}

export async function fetchRoomMembership(apiBase: string, roomId: string, token: string): Promise<LobbyMembership> {
  return requestJson(`${apiBase}/rooms/${encodeURIComponent(roomId)}/membership`, {
    headers: authHeaders(token)
  });
}

export async function joinRoom(apiBase: string, roomId: string, token: string): Promise<JoinResult> {
  return requestJson(`${apiBase}/rooms/${encodeURIComponent(roomId)}/join`, {
    method: "POST",
    headers: authHeaders(token),
    body: "{}"
  });
}

export async function leaveRoom(apiBase: string, roomId: string, token: string): Promise<LeaveResult> {
  return requestJson(`${apiBase}/rooms/${encodeURIComponent(roomId)}/leave`, {
    method: "POST",
    headers: authHeaders(token),
    body: "{}"
  });
}

export async function fetchLobbyMembership(apiBase: string, lobbyId: string, token: string): Promise<LobbyMembership> {
  return requestJson(`${apiBase}/lobbies/${encodeURIComponent(lobbyId)}/membership`, {
    headers: authHeaders(token)
  });
}

export async function joinLobby(apiBase: string, lobbyId: string, token: string): Promise<JoinResult> {
  return requestJson(`${apiBase}/lobbies/${encodeURIComponent(lobbyId)}/join`, {
    method: "POST",
    headers: authHeaders(token),
    body: JSON.stringify({ track: "manual" })
  });
}

export async function leaveLobby(apiBase: string, lobbyId: string, token: string): Promise<LeaveResult> {
  return requestJson(`${apiBase}/lobbies/${encodeURIComponent(lobbyId)}/leave`, {
    method: "POST",
    headers: authHeaders(token),
    body: "{}"
  });
}

export async function fetchSymbols(apiBase: string, scope?: MarketScope): Promise<string[]> {
  const data = await requestJson<{ symbols: string[] }>(`${scopedBase(apiBase, scope)}/symbols`);
  return data.symbols;
}

export async function fetchBook(apiBase: string, scope: MarketScope | undefined, symbol: string, depth: number): Promise<BookSnapshot> {
  return requestJson(`${scopedBase(apiBase, scope)}/book/${encodeURIComponent(symbol)}?depth=${depth}`);
}

export async function fetchPrice(apiBase: string, scope: MarketScope | undefined, symbol: string): Promise<MarketPrice> {
  return requestJson(`${scopedBase(apiBase, scope)}/prices/${encodeURIComponent(symbol)}`);
}

export async function fetchMarketTrades(
  apiBase: string,
  scope: MarketScope | undefined,
  symbol: string,
  limit = 25
): Promise<MarketTradeRecord[]> {
  const data = await requestJson<{ trades: MarketTradeRecord[] }>(
    `${scopedBase(apiBase, scope)}/trades/${encodeURIComponent(symbol)}?limit=${limit}`
  );
  return data.trades;
}

export async function fetchLeaderboard(apiBase: string, lobbyId: string, token: string): Promise<LeaderboardRow[]> {
  const data = await requestJson<{ leaderboard: LeaderboardRow[] }>(
    `${apiBase}/lobbies/${encodeURIComponent(lobbyId)}/leaderboard`,
    { headers: authHeaders(token) }
  );
  return data.leaderboard;
}

function endpointFor(side: Side, mode: OrderMode): string {
  if (mode === "limit") {
    return `/orders/${side}`;
  }

  return `/orders/${mode}-${side}`;
}

export async function submitOrder(
  apiBase: string,
  scope: MarketScope | undefined,
  token: string,
  side: Side,
  mode: OrderMode,
  order: NewOrderRequest
): Promise<SubmitResult> {
  return requestJson(`${scopedBase(apiBase, scope)}${endpointFor(side, mode)}`, {
    method: "POST",
    headers: authHeaders(token),
    body: JSON.stringify(order)
  });
}

export async function replaceOrder(
  apiBase: string,
  scope: MarketScope | undefined,
  token: string,
  side: Side,
  order: ReplaceOrderRequest
): Promise<SubmitResult> {
  return requestJson(`${scopedBase(apiBase, scope)}/orders/replace-${side}`, {
    method: "POST",
    headers: authHeaders(token),
    body: JSON.stringify(order)
  });
}

export async function cancelOrder(
  apiBase: string,
  scope: MarketScope | undefined,
  token: string,
  symbol: string,
  orderId: number
): Promise<{ canceled: boolean }> {
  return requestJson(`${scopedBase(apiBase, scope)}/orders/cancel`, {
    method: "POST",
    headers: authHeaders(token),
    body: JSON.stringify({ symbol, orderId })
  });
}

export async function fetchMe(apiBase: string, scope: MarketScope | undefined, token: string): Promise<MeSummary> {
  return requestJson(`${scopedBase(apiBase, scope)}/me`, {
    headers: authHeaders(token)
  });
}

export async function fetchOpenOrders(apiBase: string, scope: MarketScope | undefined, token: string): Promise<OpenOrder[]> {
  const data = await requestJson<{ orders: OpenOrder[] }>(`${scopedBase(apiBase, scope)}/me/orders`, {
    headers: authHeaders(token)
  });
  return data.orders;
}

export async function fetchFills(apiBase: string, scope: MarketScope | undefined, token: string): Promise<FillRecord[]> {
  const data = await requestJson<{ fills: FillRecord[] }>(`${scopedBase(apiBase, scope)}/me/fills`, {
    headers: authHeaders(token)
  });
  return data.fills;
}

export async function fetchPositions(apiBase: string, scope: MarketScope | undefined, token: string): Promise<PositionRecord[]> {
  const data = await requestJson<{ positions: PositionRecord[] }>(`${scopedBase(apiBase, scope)}/me/positions`, {
    headers: authHeaders(token)
  });
  return data.positions;
}

export async function fetchPortfolio(apiBase: string, scope: MarketScope | undefined, token: string): Promise<PortfolioRecord> {
  return requestJson(`${scopedBase(apiBase, scope)}/me/portfolio`, {
    headers: authHeaders(token)
  });
}
