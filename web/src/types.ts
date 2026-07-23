export type Side = "buy" | "sell";
export type OrderMode = "limit" | "market" | "ioc" | "fok";
export type RoomMode = "single" | "competitive";

export interface RoomAsset {
  symbol: string;
  displayName: string;
  behavior: string;
  source: string;
  referencePrice: number;
  signalQuality: string;
}

export interface GameRoom {
  id: string;
  name: string;
  mode: RoomMode;
  difficulty: string;
  startingCash: number;
  maxParticipants: number;
  houseLiquidity: boolean;
  assets: RoomAsset[];
}

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
  averageEntryPrice: number;
}

export interface MarketPrice {
  symbol: string;
  tradeCount: number;
  hasPrice: boolean;
  lastPrice: number;
  averagePrice3: number;
  averagePrice5: number;
  averagePrice10: number;
}

export interface MarketTradeRecord {
  sequence: number;
  symbol: string;
  takerId: number;
  makerId: number;
  takerTraderId: number;
  makerTraderId: number;
  takerSide: Side;
  price: number;
  quantity: number;
  notional: number;
}

export interface PortfolioPositionRecord extends PositionRecord {
  hasMark: boolean;
  markPrice: number;
  marketValue: number;
  costBasisValue: number;
  unrealizedPnl: number;
}

export interface PortfolioRecord {
  traderId: number;
  startingCash: number;
  cashFlow: number;
  marketValue: number;
  estimatedValue: number;
  tradingPnl: number;
  unrealizedPnl: number;
  positions: PortfolioPositionRecord[];
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
