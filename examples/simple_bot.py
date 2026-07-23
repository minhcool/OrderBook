#!/usr/bin/env python3
"""Tiny HTTP bot for the Orderbook prototype.

The bot joins a single-player room by default, advances the simulator, reads the
book and portfolio, then alternates small IOC buys/sells against displayed
liquidity. Set ORDERBOOK_BOT_TOKEN to a configured bot API key for deployed
testing. It uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import time
import urllib.error
import urllib.request
from typing import Any


def dev_token(subject: str) -> str:
    header = base64.urlsafe_b64encode(b"{}").decode("ascii").rstrip("=")
    payload = base64.urlsafe_b64encode(
        json.dumps({"sub": subject}, separators=(",", ":")).encode("utf-8")
    ).decode("ascii").rstrip("=")
    return f"{header}.{payload}.dev"


def request_json(method: str, url: str, token: str | None = None, body: dict[str, Any] | None = None) -> dict[str, Any]:
    data = None if body is None else json.dumps(body).encode("utf-8")
    headers = {"Accept": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if body is not None:
        headers["Content-Type"] = "application/json"

    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            raw = response.read().decode("utf-8")
            return json.loads(raw) if raw else {}
    except urllib.error.HTTPError as error:
        raw = error.read().decode("utf-8")
        message = raw
        try:
            parsed = json.loads(raw)
            message = parsed.get("error", raw)
        except json.JSONDecodeError:
            pass
        raise RuntimeError(f"{method} {url} failed with {error.code}: {message}") from error


def position_quantity(portfolio: dict[str, Any], symbol: str) -> int:
    for position in portfolio.get("positions", []):
        if position.get("symbol") == symbol:
            return int(position.get("quantity", 0))
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(description="Run a minimal Orderbook HTTP bot.")
    parser.add_argument("--api", default=os.getenv("ORDERBOOK_API_URL", "http://localhost:8080"))
    parser.add_argument("--mode", choices=["single", "competitive"], default="single")
    parser.add_argument("--room", default="solo-alpha")
    parser.add_argument("--lobby", default="aurora-open-10")
    parser.add_argument("--symbol", default="")
    parser.add_argument("--quantity", type=int, default=1)
    parser.add_argument("--iterations", type=int, default=10)
    parser.add_argument("--sleep", type=float, default=1.0)
    parser.add_argument("--steps", type=int, default=1)
    parser.add_argument("--token", default=os.getenv("ORDERBOOK_BOT_TOKEN") or os.getenv("CLERK_TOKEN"))
    parser.add_argument("--dev-sub", default=os.getenv("ORDERBOOK_BOT_SUB", "local-bot"))
    args = parser.parse_args()

    token = args.token or dev_token(args.dev_sub)
    api = args.api.rstrip("/")
    scope = f"/rooms/{args.room}" if args.mode == "single" else f"/lobbies/{args.lobby}"

    if args.mode == "single":
        request_json("POST", f"{api}/rooms/{args.room}/join", token, {})
    else:
        request_json("POST", f"{api}/lobbies/{args.lobby}/join", token, {"track": "bot"})

    symbols = request_json("GET", f"{api}{scope}/symbols", token).get("symbols", [])
    symbol = args.symbol or (symbols[0] if symbols else "")
    if not symbol:
        raise RuntimeError("No tradable symbols returned by the API.")

    print(f"bot joined {scope}, trading {symbol}")

    for index in range(args.iterations):
        if args.mode == "single":
            request_json("POST", f"{api}/rooms/{args.room}/simulator/tick", token, {"steps": args.steps})

        book = request_json("GET", f"{api}{scope}/book/{symbol}?depth=3", token)
        portfolio = request_json("GET", f"{api}{scope}/me/portfolio", token)
        bids = book.get("bids", [])
        asks = book.get("asks", [])
        quantity = max(1, args.quantity)
        held = position_quantity(portfolio, symbol)

        action = "hold"
        if asks and (index % 2 == 0 or held <= 0):
            ask = int(asks[0]["price"])
            required_cash = ask * quantity
            if int(portfolio.get("availableCash", 0)) >= required_cash:
                result = request_json(
                    "POST",
                    f"{api}{scope}/orders/ioc-buy",
                    token,
                    {"symbol": symbol, "price": ask, "quantity": quantity},
                )
                action = f"buy #{result.get('orderId')} filled {result.get('filledQuantity')}"
        elif bids and held > 0:
            bid = int(bids[0]["price"])
            sell_qty = min(quantity, held)
            result = request_json(
                "POST",
                f"{api}{scope}/orders/ioc-sell",
                token,
                {"symbol": symbol, "price": bid, "quantity": sell_qty},
            )
            action = f"sell #{result.get('orderId')} filled {result.get('filledQuantity')}"

        best_bid = bids[0]["price"] if bids else "-"
        best_ask = asks[0]["price"] if asks else "-"
        estimated_value = portfolio.get("estimatedValue", 0)
        print(f"{index + 1:02d} bid={best_bid} ask={best_ask} held={held} value={estimated_value} action={action}")
        if args.sleep > 0:
            time.sleep(args.sleep)

    fills = request_json("GET", f"{api}{scope}/me/fills", token).get("fills", [])
    print(f"fills={len(fills)}")


if __name__ == "__main__":
    main()
