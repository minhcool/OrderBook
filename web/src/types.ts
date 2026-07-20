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

export interface OrderRequest {
  symbol: string;
  orderId: number;
  price?: number;
  quantity: number;
}
