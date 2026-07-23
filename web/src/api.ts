import type {
  BookSnapshot,
  FillRecord,
  MarketPrice,
  MarketTradeRecord,
  MeSummary,
  NewOrderRequest,
  OpenOrder,
  OrderMode,
  PortfolioRecord,
  PositionRecord,
  ReplaceOrderRequest,
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

export async function fetchSymbols(apiBase: string): Promise<string[]> {
  const data = await requestJson<{ symbols: string[] }>(`${apiBase}/symbols`);
  return data.symbols;
}

export async function fetchBook(apiBase: string, symbol: string, depth: number): Promise<BookSnapshot> {
  return requestJson(`${apiBase}/book/${encodeURIComponent(symbol)}?depth=${depth}`);
}

export async function fetchPrice(apiBase: string, symbol: string): Promise<MarketPrice> {
  return requestJson(`${apiBase}/prices/${encodeURIComponent(symbol)}`);
}

export async function fetchMarketTrades(apiBase: string, symbol: string, limit = 25): Promise<MarketTradeRecord[]> {
  const data = await requestJson<{ trades: MarketTradeRecord[] }>(
    `${apiBase}/trades/${encodeURIComponent(symbol)}?limit=${limit}`
  );
  return data.trades;
}

function endpointFor(side: Side, mode: OrderMode): string {
  if (mode === "limit") {
    return `/orders/${side}`;
  }

  return `/orders/${mode}-${side}`;
}

export async function submitOrder(
  apiBase: string,
  token: string,
  side: Side,
  mode: OrderMode,
  order: NewOrderRequest
): Promise<SubmitResult> {
  return requestJson(`${apiBase}${endpointFor(side, mode)}`, {
    method: "POST",
    headers: authHeaders(token),
    body: JSON.stringify(order)
  });
}

export async function replaceOrder(
  apiBase: string,
  token: string,
  side: Side,
  order: ReplaceOrderRequest
): Promise<SubmitResult> {
  return requestJson(`${apiBase}/orders/replace-${side}`, {
    method: "POST",
    headers: authHeaders(token),
    body: JSON.stringify(order)
  });
}

export async function cancelOrder(
  apiBase: string,
  token: string,
  symbol: string,
  orderId: number
): Promise<{ canceled: boolean }> {
  return requestJson(`${apiBase}/orders/cancel`, {
    method: "POST",
    headers: authHeaders(token),
    body: JSON.stringify({ symbol, orderId })
  });
}

export async function fetchMe(apiBase: string, token: string): Promise<MeSummary> {
  return requestJson(`${apiBase}/me`, {
    headers: authHeaders(token)
  });
}

export async function fetchOpenOrders(apiBase: string, token: string): Promise<OpenOrder[]> {
  const data = await requestJson<{ orders: OpenOrder[] }>(`${apiBase}/me/orders`, {
    headers: authHeaders(token)
  });
  return data.orders;
}

export async function fetchFills(apiBase: string, token: string): Promise<FillRecord[]> {
  const data = await requestJson<{ fills: FillRecord[] }>(`${apiBase}/me/fills`, {
    headers: authHeaders(token)
  });
  return data.fills;
}

export async function fetchPositions(apiBase: string, token: string): Promise<PositionRecord[]> {
  const data = await requestJson<{ positions: PositionRecord[] }>(`${apiBase}/me/positions`, {
    headers: authHeaders(token)
  });
  return data.positions;
}

export async function fetchPortfolio(apiBase: string, token: string): Promise<PortfolioRecord> {
  return requestJson(`${apiBase}/me/portfolio`, {
    headers: authHeaders(token)
  });
}
