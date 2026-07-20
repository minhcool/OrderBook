import type { BookSnapshot, OrderMode, OrderRequest, Side, SubmitResult } from "./types";

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

function endpointFor(side: Side, mode: OrderMode): string {
  if (mode === "limit") {
    return `/orders/${side}`;
  }

  return `/orders/${mode}-${side}`;
}

export async function submitOrder(
  apiBase: string,
  side: Side,
  mode: OrderMode,
  order: OrderRequest
): Promise<SubmitResult> {
  return requestJson(`${apiBase}${endpointFor(side, mode)}`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json"
    },
    body: JSON.stringify(order)
  });
}

export async function replaceOrder(
  apiBase: string,
  side: Side,
  order: Required<OrderRequest>
): Promise<SubmitResult> {
  return requestJson(`${apiBase}/orders/replace-${side}`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json"
    },
    body: JSON.stringify(order)
  });
}

export async function cancelOrder(apiBase: string, symbol: string, orderId: number): Promise<{ canceled: boolean }> {
  return requestJson(`${apiBase}/orders/cancel`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json"
    },
    body: JSON.stringify({ symbol, orderId })
  });
}
