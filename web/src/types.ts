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
