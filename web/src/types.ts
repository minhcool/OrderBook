export type Side = "buy" | "sell";
export type OrderMode = "limit" | "market" | "ioc" | "fok";

export interface Trade {
  takerId: number;
  makerId: number;
  takerTraderId: number;
  makerTraderId: number;
  takerSide: Side;
  price: number;
  quantity: number;
}

export interface SubmitResult {
  orderId: number;
  accepted: boolean;
  filledQuantity: number;
  restingQuantity: number;
  notional: number;
  averagePrice: number;
  selfTradePrevented: boolean;
  message: string;
  trades: Trade[];
}

export interface BookLevel {
  price: number;
  quantity: number;
}

export interface BookSnapshot {
  symbol: string;
  bids: BookLevel[];
  asks: BookLevel[];
}

export interface OpenOrder {
  symbol: string;
  orderId: number;
  side: Side;
  price: number;
  quantity: number;
  remainingQuantity: number;
}

export interface FillRecord {
  sequence: number;
  symbol: string;
  orderId: number;
  counterpartyOrderId: number;
  side: Side;
  liquidity: "maker" | "taker";
  price: number;
  quantity: number;
  notional: number;
}

export interface PositionRecord {
  symbol: string;
  quantity: number;
  quoteCashFlow: number;
  boughtQuantity: number;
  soldQuantity: number;
}

export interface MeSummary {
  traderId: number;
  openOrderCount: number;
  fillCount: number;
  positionCount: number;
}

export interface NewOrderRequest {
  symbol: string;
  price?: number;
  quantity: number;
}

export interface ReplaceOrderRequest {
  symbol: string;
  orderId: number;
  price: number;
  quantity: number;
}
