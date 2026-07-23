#include "orderbook/exchange.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle InvalidSocket = -1;
#endif

namespace {

struct HttpRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body = "{}";
};

class AuthError : public std::runtime_error {
public:
    explicit AuthError(const std::string& message)
        : std::runtime_error(message) {}
};

class OrderIdGenerator {
public:
    OrderId next() {
        return nextId.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::atomic<OrderId> nextId{1};
};

struct ParsedOrder {
    std::string symbol;
    OrderId orderId = 0;
    Price price = 0;
    Qty quantity = 0;
};

struct FillRecord {
    std::uint64_t sequence = 0;
    std::string symbol;
    OrderId orderId = 0;
    OrderId counterpartyOrderId = 0;
    TraderId traderId = 0;
    TraderId counterpartyTraderId = 0;
    Side side = Side::Buy;
    std::string liquidity;
    Price price = 0;
    Qty quantity = 0;
    std::int64_t notional = 0;
};

struct PositionRecord {
    std::string symbol;
    Qty quantity = 0;
    std::int64_t quoteCashFlow = 0;
    Qty boughtQuantity = 0;
    Qty soldQuantity = 0;
    double averageEntryPrice = 0.0;
};

struct MarketTradeRecord {
    std::uint64_t sequence = 0;
    std::string symbol;
    OrderId takerId = 0;
    OrderId makerId = 0;
    TraderId takerTraderId = 0;
    TraderId makerTraderId = 0;
    Side takerSide = Side::Buy;
    Price price = 0;
    Qty quantity = 0;
    std::int64_t notional = 0;
};

struct PriceRecord {
    std::string symbol;
    std::size_t tradeCount = 0;
    bool hasPrice = false;
    Price lastPrice = 0;
    double averagePrice3 = 0.0;
    double averagePrice5 = 0.0;
    double averagePrice10 = 0.0;
};

struct PortfolioPositionRecord {
    PositionRecord position;
    bool hasMark = false;
    double markPrice = 0.0;
    double marketValue = 0.0;
    double costBasisValue = 0.0;
    double unrealizedPnl = 0.0;
};

struct PortfolioRecord {
    TraderId traderId = 0;
    std::int64_t cashFlow = 0;
    double marketValue = 0.0;
    double estimatedValue = 0.0;
    double unrealizedPnl = 0.0;
    std::vector<PortfolioPositionRecord> positions;
};

class AccountStore {
public:
    void recordTrades(const std::string& symbol, const SubmitResult& result) {
        std::lock_guard<std::mutex> lock(accountMutex);

        for (const Trade& trade : result.trades) {
            appendMarketTrade(symbol, trade);

            appendFill(
                symbol,
                trade.takerId,
                trade.makerId,
                trade.takerTraderId,
                trade.makerTraderId,
                trade.takerSide,
                "taker",
                trade.price,
                trade.quantity);

            appendFill(
                symbol,
                trade.makerId,
                trade.takerId,
                trade.makerTraderId,
                trade.takerTraderId,
                opposite(trade.takerSide),
                "maker",
                trade.price,
                trade.quantity);
        }
    }

    std::vector<MarketTradeRecord> tradesForSymbol(const std::string& symbol, std::size_t limit) const {
        std::lock_guard<std::mutex> lock(accountMutex);

        std::vector<MarketTradeRecord> result;
        for (auto it = marketTrades.rbegin(); it != marketTrades.rend(); ++it) {
            if (it->symbol != symbol) {
                continue;
            }

            result.push_back(*it);
            if (result.size() >= limit) {
                break;
            }
        }

        return result;
    }

    PriceRecord priceForSymbol(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(accountMutex);
        return priceForSymbolLocked(symbol);
    }

    std::vector<PriceRecord> pricesForSymbols(const std::vector<std::string>& symbols) const {
        std::lock_guard<std::mutex> lock(accountMutex);

        std::vector<PriceRecord> result;
        result.reserve(symbols.size());
        for (const std::string& symbol : symbols) {
            result.push_back(priceForSymbolLocked(symbol));
        }

        return result;
    }

    std::vector<FillRecord> fillsForTrader(TraderId traderId) const {
        std::lock_guard<std::mutex> lock(accountMutex);

        std::vector<FillRecord> result;
        for (const FillRecord& fill : fills) {
            if (fill.traderId == traderId) {
                result.push_back(fill);
            }
        }

        std::sort(result.begin(), result.end(), [](const FillRecord& left, const FillRecord& right) {
            return left.sequence > right.sequence;
        });
        return result;
    }

    std::vector<PositionRecord> positionsForTrader(TraderId traderId) const {
        std::lock_guard<std::mutex> lock(accountMutex);
        return positionsForTraderLocked(traderId);
    }

    PortfolioRecord portfolioForTrader(TraderId traderId) const {
        std::lock_guard<std::mutex> lock(accountMutex);

        PortfolioRecord portfolio;
        portfolio.traderId = traderId;
        const std::vector<PositionRecord> positions = positionsForTraderLocked(traderId);
        portfolio.positions.reserve(positions.size());

        for (const PositionRecord& position : positions) {
            PortfolioPositionRecord marked;
            marked.position = position;

            portfolio.cashFlow += position.quoteCashFlow;

            const PriceRecord price = priceForSymbolLocked(position.symbol);
            if (price.hasPrice) {
                marked.hasMark = true;
                marked.markPrice = static_cast<double>(price.lastPrice);
                marked.marketValue = static_cast<double>(position.quantity) * marked.markPrice;
                marked.costBasisValue = static_cast<double>(position.quantity) * position.averageEntryPrice;
                marked.unrealizedPnl = marked.marketValue - marked.costBasisValue;

                portfolio.marketValue += marked.marketValue;
                portfolio.unrealizedPnl += marked.unrealizedPnl;
            }

            portfolio.positions.push_back(marked);
        }

        portfolio.estimatedValue = static_cast<double>(portfolio.cashFlow) + portfolio.marketValue;
        return portfolio;
    }

private:
    struct MutablePosition {
        PositionRecord record;
        Qty openQuantity = 0;
        double averageEntryPrice = 0.0;
    };

    static Side opposite(Side side) {
        return side == Side::Buy ? Side::Sell : Side::Buy;
    }

    static double vwapFromLatest(const std::vector<MarketTradeRecord>& trades, const std::string& symbol, std::size_t count) {
        std::int64_t notional = 0;
        Qty quantity = 0;
        std::size_t seen = 0;

        for (auto it = trades.rbegin(); it != trades.rend() && seen < count; ++it) {
            if (it->symbol != symbol) {
                continue;
            }

            notional += it->notional;
            quantity += it->quantity;
            ++seen;
        }

        if (quantity == 0) {
            return 0.0;
        }

        return static_cast<double>(notional) / static_cast<double>(quantity);
    }

    PriceRecord priceForSymbolLocked(const std::string& symbol) const {
        PriceRecord price;
        price.symbol = symbol;

        for (auto it = marketTrades.rbegin(); it != marketTrades.rend(); ++it) {
            if (it->symbol != symbol) {
                continue;
            }

            if (!price.hasPrice) {
                price.hasPrice = true;
                price.lastPrice = it->price;
            }

            ++price.tradeCount;
        }

        if (price.hasPrice) {
            price.averagePrice3 = vwapFromLatest(marketTrades, symbol, 3);
            price.averagePrice5 = vwapFromLatest(marketTrades, symbol, 5);
            price.averagePrice10 = vwapFromLatest(marketTrades, symbol, 10);
        }

        return price;
    }

    std::vector<PositionRecord> positionsForTraderLocked(TraderId traderId) const {
        std::map<std::string, PositionRecord> bySymbol;
        std::map<std::string, MutablePosition> openPositions;
        for (const FillRecord& fill : fills) {
            if (fill.traderId != traderId) {
                continue;
            }

            PositionRecord& position = bySymbol[fill.symbol];
            position.symbol = fill.symbol;

            if (fill.side == Side::Buy) {
                position.quantity += fill.quantity;
                position.quoteCashFlow -= fill.notional;
                position.boughtQuantity += fill.quantity;
            } else {
                position.quantity -= fill.quantity;
                position.quoteCashFlow += fill.notional;
                position.soldQuantity += fill.quantity;
            }

            applyEntryPrice(openPositions[fill.symbol], fill);
        }

        std::vector<PositionRecord> result;
        result.reserve(bySymbol.size());
        for (const auto& [symbol, position] : bySymbol) {
            PositionRecord enriched = position;
            const auto openPosition = openPositions.find(symbol);
            if (openPosition != openPositions.end()) {
                enriched.averageEntryPrice = openPosition->second.averageEntryPrice;
            }

            result.push_back(enriched);
        }

        return result;
    }

    static void applyEntryPrice(MutablePosition& position, const FillRecord& fill) {
        const Qty signedQuantity = fill.side == Side::Buy ? fill.quantity : -fill.quantity;
        const double fillPrice = static_cast<double>(fill.price);

        if (position.openQuantity == 0) {
            position.openQuantity = signedQuantity;
            position.averageEntryPrice = fillPrice;
            return;
        }

        const bool sameDirection = (position.openQuantity > 0 && signedQuantity > 0)
            || (position.openQuantity < 0 && signedQuantity < 0);
        if (sameDirection) {
            const double currentAbs = static_cast<double>(std::abs(position.openQuantity));
            const double incomingAbs = static_cast<double>(std::abs(signedQuantity));
            position.averageEntryPrice =
                ((currentAbs * position.averageEntryPrice) + (incomingAbs * fillPrice)) / (currentAbs + incomingAbs);
            position.openQuantity += signedQuantity;
            return;
        }

        const Qty updatedQuantity = position.openQuantity + signedQuantity;
        if (updatedQuantity == 0) {
            position.openQuantity = 0;
            position.averageEntryPrice = 0.0;
            return;
        }

        const bool flippedDirection = (position.openQuantity > 0 && updatedQuantity < 0)
            || (position.openQuantity < 0 && updatedQuantity > 0);
        position.openQuantity = updatedQuantity;
        if (flippedDirection) {
            position.averageEntryPrice = fillPrice;
        }
    }

    void appendMarketTrade(const std::string& symbol, const Trade& trade) {
        marketTrades.push_back({
            nextMarketSequence++,
            symbol,
            trade.takerId,
            trade.makerId,
            trade.takerTraderId,
            trade.makerTraderId,
            trade.takerSide,
            trade.price,
            trade.quantity,
            trade.price * trade.quantity,
        });
    }

    void appendFill(
        const std::string& symbol,
        OrderId orderId,
        OrderId counterpartyOrderId,
        TraderId traderId,
        TraderId counterpartyTraderId,
        Side side,
        const std::string& liquidity,
        Price price,
        Qty quantity) {
        fills.push_back({
            nextFillSequence++,
            symbol,
            orderId,
            counterpartyOrderId,
            traderId,
            counterpartyTraderId,
            side,
            liquidity,
            price,
            quantity,
            price * quantity,
        });
    }

    mutable std::mutex accountMutex;
    std::uint64_t nextFillSequence = 1;
    std::uint64_t nextMarketSequence = 1;
    std::vector<FillRecord> fills;
    std::vector<MarketTradeRecord> marketTrades;
};

#ifdef _WIN32
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~SocketRuntime() {
        WSACleanup();
    }
};
#else
class SocketRuntime {
public:
    SocketRuntime() = default;
};
#endif

void closeSocket(SocketHandle socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

std::string trim(std::string value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();

    if (begin >= end) {
        return {};
    }

    return std::string(begin, end);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    return value;
}

std::string statusText(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 500:
            return "Internal Server Error";
        default:
            return "OK";
    }
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;

    for (char ch : value) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << ch;
                break;
        }
    }

    return out.str();
}

std::string jsonDouble(double value) {
    if (value > -0.0000005 && value < 0.0000005) {
        value = 0.0;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;

    std::string result = out.str();
    while (result.size() > 1 && result.back() == '0') {
        result.pop_back();
    }

    if (!result.empty() && result.back() == '.') {
        result.push_back('0');
    }

    return result;
}

std::string sideToString(Side side) {
    return side == Side::Buy ? "buy" : "sell";
}

std::string jsonError(const std::string& message) {
    return "{\"error\":\"" + jsonEscape(message) + "\"}";
}

std::string serializeTrades(const std::vector<Trade>& trades) {
    std::ostringstream out;
    out << "[";

    for (std::size_t i = 0; i < trades.size(); ++i) {
        const Trade& trade = trades[i];
        if (i > 0) {
            out << ",";
        }

        out << "{"
            << "\"takerId\":" << trade.takerId << ","
            << "\"makerId\":" << trade.makerId << ","
            << "\"takerTraderId\":" << trade.takerTraderId << ","
            << "\"makerTraderId\":" << trade.makerTraderId << ","
            << "\"takerSide\":\"" << sideToString(trade.takerSide) << "\","
            << "\"price\":" << trade.price << ","
            << "\"quantity\":" << trade.quantity
            << "}";
    }

    out << "]";
    return out.str();
}

std::string serializeSubmitResult(const SubmitResult& result) {
    std::ostringstream out;

    out << "{"
        << "\"orderId\":" << result.orderId << ","
        << "\"accepted\":" << (result.accepted ? "true" : "false") << ","
        << "\"filledQuantity\":" << result.filledQuantity << ","
        << "\"restingQuantity\":" << result.restingQuantity << ","
        << "\"notional\":" << result.notional << ","
        << "\"averagePrice\":" << result.averagePrice() << ","
        << "\"selfTradePrevented\":" << (result.selfTradePrevented ? "true" : "false") << ","
        << "\"message\":\"" << jsonEscape(result.message) << "\","
        << "\"trades\":" << serializeTrades(result.trades)
        << "}";

    return out.str();
}

std::string serializeLevels(const std::vector<BookLevel>& levels) {
    std::ostringstream out;
    out << "[";

    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        out << "{"
            << "\"price\":" << levels[i].price << ","
            << "\"quantity\":" << levels[i].quantity
            << "}";
    }

    out << "]";
    return out.str();
}

std::string serializeSnapshot(const std::string& symbol, const BookSnapshot& snapshot) {
    std::ostringstream out;

    out << "{"
        << "\"symbol\":\"" << jsonEscape(symbol) << "\","
        << "\"bids\":" << serializeLevels(snapshot.bids) << ","
        << "\"asks\":" << serializeLevels(snapshot.asks)
        << "}";

    return out.str();
}

std::string serializeSymbols(const std::vector<std::string>& symbols) {
    std::ostringstream out;
    out << "{\"symbols\":[";

    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        out << "\"" << jsonEscape(symbols[i]) << "\"";
    }

    out << "]}";
    return out.str();
}

std::string serializePrice(const PriceRecord& price) {
    std::ostringstream out;
    out << "{"
        << "\"symbol\":\"" << jsonEscape(price.symbol) << "\","
        << "\"tradeCount\":" << price.tradeCount << ","
        << "\"hasPrice\":" << (price.hasPrice ? "true" : "false") << ","
        << "\"lastPrice\":" << price.lastPrice << ","
        << "\"averagePrice3\":" << jsonDouble(price.averagePrice3) << ","
        << "\"averagePrice5\":" << jsonDouble(price.averagePrice5) << ","
        << "\"averagePrice10\":" << jsonDouble(price.averagePrice10)
        << "}";
    return out.str();
}

std::string serializePrices(const std::vector<PriceRecord>& prices) {
    std::ostringstream out;
    out << "{\"prices\":[";

    for (std::size_t i = 0; i < prices.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        out << serializePrice(prices[i]);
    }

    out << "]}";
    return out.str();
}

std::string serializeMarketTrades(const std::vector<MarketTradeRecord>& trades) {
    std::ostringstream out;
    out << "{\"trades\":[";

    for (std::size_t i = 0; i < trades.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        const MarketTradeRecord& trade = trades[i];
        out << "{"
            << "\"sequence\":" << trade.sequence << ","
            << "\"symbol\":\"" << jsonEscape(trade.symbol) << "\","
            << "\"takerId\":" << trade.takerId << ","
            << "\"makerId\":" << trade.makerId << ","
            << "\"takerTraderId\":" << trade.takerTraderId << ","
            << "\"makerTraderId\":" << trade.makerTraderId << ","
            << "\"takerSide\":\"" << sideToString(trade.takerSide) << "\","
            << "\"price\":" << trade.price << ","
            << "\"quantity\":" << trade.quantity << ","
            << "\"notional\":" << trade.notional
            << "}";
    }

    out << "]}";
    return out.str();
}

struct SymbolOpenOrder {
    std::string symbol;
    OpenOrder order;
};

std::vector<SymbolOpenOrder> collectOpenOrders(const Exchange& exchange, TraderId traderId) {
    std::vector<SymbolOpenOrder> result;

    for (const std::string& symbol : exchange.symbols()) {
        std::vector<OpenOrder> orders = exchange.openOrders(symbol, traderId);
        for (const OpenOrder& order : orders) {
            result.push_back({symbol, order});
        }
    }

    std::sort(result.begin(), result.end(), [](const SymbolOpenOrder& left, const SymbolOpenOrder& right) {
        return left.order.orderId < right.order.orderId;
    });
    return result;
}

std::string serializeOpenOrders(const std::vector<SymbolOpenOrder>& orders) {
    std::ostringstream out;
    out << "{\"orders\":[";

    for (std::size_t i = 0; i < orders.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        const SymbolOpenOrder& item = orders[i];
        out << "{"
            << "\"symbol\":\"" << jsonEscape(item.symbol) << "\","
            << "\"orderId\":" << item.order.orderId << ","
            << "\"side\":\"" << sideToString(item.order.side) << "\","
            << "\"price\":" << item.order.price << ","
            << "\"quantity\":" << item.order.quantity << ","
            << "\"remainingQuantity\":" << item.order.remainingQuantity
            << "}";
    }

    out << "]}";
    return out.str();
}

std::string serializeFills(const std::vector<FillRecord>& fills) {
    std::ostringstream out;
    out << "{\"fills\":[";

    for (std::size_t i = 0; i < fills.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        const FillRecord& fill = fills[i];
        out << "{"
            << "\"sequence\":" << fill.sequence << ","
            << "\"symbol\":\"" << jsonEscape(fill.symbol) << "\","
            << "\"orderId\":" << fill.orderId << ","
            << "\"counterpartyOrderId\":" << fill.counterpartyOrderId << ","
            << "\"side\":\"" << sideToString(fill.side) << "\","
            << "\"liquidity\":\"" << jsonEscape(fill.liquidity) << "\","
            << "\"price\":" << fill.price << ","
            << "\"quantity\":" << fill.quantity << ","
            << "\"notional\":" << fill.notional
            << "}";
    }

    out << "]}";
    return out.str();
}

std::string serializePositions(const std::vector<PositionRecord>& positions) {
    std::ostringstream out;
    out << "{\"positions\":[";

    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        const PositionRecord& position = positions[i];
        out << "{"
            << "\"symbol\":\"" << jsonEscape(position.symbol) << "\","
            << "\"quantity\":" << position.quantity << ","
            << "\"quoteCashFlow\":" << position.quoteCashFlow << ","
            << "\"boughtQuantity\":" << position.boughtQuantity << ","
            << "\"soldQuantity\":" << position.soldQuantity << ","
            << "\"averageEntryPrice\":" << jsonDouble(position.averageEntryPrice)
            << "}";
    }

    out << "]}";
    return out.str();
}

std::string serializePortfolioPosition(const PortfolioPositionRecord& item) {
    const PositionRecord& position = item.position;
    std::ostringstream out;
    out << "{"
        << "\"symbol\":\"" << jsonEscape(position.symbol) << "\","
        << "\"quantity\":" << position.quantity << ","
        << "\"quoteCashFlow\":" << position.quoteCashFlow << ","
        << "\"boughtQuantity\":" << position.boughtQuantity << ","
        << "\"soldQuantity\":" << position.soldQuantity << ","
        << "\"averageEntryPrice\":" << jsonDouble(position.averageEntryPrice) << ","
        << "\"hasMark\":" << (item.hasMark ? "true" : "false") << ","
        << "\"markPrice\":" << jsonDouble(item.markPrice) << ","
        << "\"marketValue\":" << jsonDouble(item.marketValue) << ","
        << "\"costBasisValue\":" << jsonDouble(item.costBasisValue) << ","
        << "\"unrealizedPnl\":" << jsonDouble(item.unrealizedPnl)
        << "}";
    return out.str();
}

std::string serializePortfolio(const PortfolioRecord& portfolio) {
    std::ostringstream out;
    out << "{"
        << "\"traderId\":" << portfolio.traderId << ","
        << "\"cashFlow\":" << portfolio.cashFlow << ","
        << "\"marketValue\":" << jsonDouble(portfolio.marketValue) << ","
        << "\"estimatedValue\":" << jsonDouble(portfolio.estimatedValue) << ","
        << "\"unrealizedPnl\":" << jsonDouble(portfolio.unrealizedPnl) << ","
        << "\"positions\":[";

    for (std::size_t i = 0; i < portfolio.positions.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        out << serializePortfolioPosition(portfolio.positions[i]);
    }

    out << "]}";
    return out.str();
}

std::string serializeMe(
    TraderId traderId,
    const std::vector<SymbolOpenOrder>& orders,
    const std::vector<FillRecord>& fills,
    const std::vector<PositionRecord>& positions) {
    std::ostringstream out;
    out << "{"
        << "\"traderId\":" << traderId << ","
        << "\"openOrderCount\":" << orders.size() << ","
        << "\"fillCount\":" << fills.size() << ","
        << "\"positionCount\":" << positions.size()
        << "}";
    return out.str();
}

std::optional<std::string> jsonStringField(const std::string& body, const std::string& field) {
    const std::regex pattern("\"" + field + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;

    if (!std::regex_search(body, match, pattern)) {
        return std::nullopt;
    }

    return match[1].str();
}

std::optional<std::int64_t> jsonIntField(const std::string& body, const std::string& field) {
    const std::regex pattern("\"" + field + "\"\\s*:\\s*(-?[0-9]+)");
    std::smatch match;

    if (!std::regex_search(body, match, pattern)) {
        return std::nullopt;
    }

    return std::stoll(match[1].str());
}

std::optional<std::string> bearerToken(const HttpRequest& request) {
    const auto header = request.headers.find("authorization");
    if (header == request.headers.end()) {
        return std::nullopt;
    }

    const std::string value = trim(header->second);
    const std::string prefix = "Bearer ";
    if (value.size() <= prefix.size() || value.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }

    return value.substr(prefix.size());
}

int base64UrlValue(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }

    if (ch >= 'a' && ch <= 'z') {
        return 26 + ch - 'a';
    }

    if (ch >= '0' && ch <= '9') {
        return 52 + ch - '0';
    }

    if (ch == '-' || ch == '+') {
        return 62;
    }

    if (ch == '_' || ch == '/') {
        return 63;
    }

    return -1;
}

std::string base64UrlDecode(const std::string& value) {
    std::string output;
    int bits = 0;
    int bitCount = 0;

    for (char ch : value) {
        if (ch == '=') {
            break;
        }

        const int decoded = base64UrlValue(ch);
        if (decoded < 0) {
            continue;
        }

        bits = (bits << 6) | decoded;
        bitCount += 6;

        if (bitCount >= 8) {
            bitCount -= 8;
            output.push_back(static_cast<char>((bits >> bitCount) & 0xff));
        }
    }

    return output;
}

std::optional<std::string> jwtSubjectWithoutVerification(const std::string& token) {
    const std::size_t firstDot = token.find('.');
    if (firstDot == std::string::npos) {
        return std::nullopt;
    }

    const std::size_t secondDot = token.find('.', firstDot + 1);
    if (secondDot == std::string::npos) {
        return std::nullopt;
    }

    const std::string payload = base64UrlDecode(token.substr(firstDot + 1, secondDot - firstDot - 1));
    return jsonStringField(payload, "sub");
}

TraderId traderIdFromSubject(const std::string& subject) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : subject) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }

    return static_cast<TraderId>((hash % 9'000'000'000ULL) + 1);
}

TraderId authenticatedTraderId(const HttpRequest& request) {
    const std::optional<std::string> token = bearerToken(request);
    if (!token) {
        throw AuthError("missing Authorization bearer token");
    }

    const std::optional<std::string> subject = jwtSubjectWithoutVerification(*token);
    if (!subject || subject->empty()) {
        throw AuthError("bearer token must be a Clerk JWT with a sub claim");
    }

    return traderIdFromSubject(*subject);
}

void rejectClientOrderId(const std::string& body) {
    if (jsonIntField(body, "orderId")) {
        throw std::runtime_error("orderId is assigned by the server");
    }
}

ParsedOrder parseNewOrderWithPrice(const std::string& body) {
    rejectClientOrderId(body);

    ParsedOrder order;

    const auto symbol = jsonStringField(body, "symbol");
    const auto price = jsonIntField(body, "price");
    const auto quantity = jsonIntField(body, "quantity");

    if (!symbol || !price || !quantity) {
        throw std::runtime_error("expected symbol, price, and quantity");
    }

    order.symbol = *symbol;
    order.price = *price;
    order.quantity = *quantity;

    return order;
}

ParsedOrder parseNewMarketOrder(const std::string& body) {
    rejectClientOrderId(body);

    ParsedOrder order;

    const auto symbol = jsonStringField(body, "symbol");
    const auto quantity = jsonIntField(body, "quantity");

    if (!symbol || !quantity) {
        throw std::runtime_error("expected symbol and quantity");
    }

    order.symbol = *symbol;
    order.quantity = *quantity;

    return order;
}

ParsedOrder parseOrderWithPrice(const std::string& body) {
    ParsedOrder order;

    const auto symbol = jsonStringField(body, "symbol");
    const auto orderId = jsonIntField(body, "orderId");
    const auto price = jsonIntField(body, "price");
    const auto quantity = jsonIntField(body, "quantity");

    if (!symbol || !orderId || !price || !quantity) {
        throw std::runtime_error("expected symbol, orderId, price, and quantity");
    }

    order.symbol = *symbol;
    order.orderId = *orderId;
    order.price = *price;
    order.quantity = *quantity;

    return order;
}

std::pair<std::string, std::string> splitQuery(const std::string& target) {
    const std::size_t queryStart = target.find('?');
    if (queryStart == std::string::npos) {
        return {target, ""};
    }

    return {target.substr(0, queryStart), target.substr(queryStart + 1)};
}

int fromHex(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }

    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }

    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }

    return -1;
}

std::string urlDecode(const std::string& value) {
    std::string result;

    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = fromHex(value[i + 1]);
            const int low = fromHex(value[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }

        result.push_back(value[i] == '+' ? ' ' : value[i]);
    }

    return result;
}

std::size_t parseDepth(const std::string& query) {
    const std::regex pattern("(?:^|&)depth=([0-9]+)(?:&|$)");
    std::smatch match;

    if (!std::regex_search(query, match, pattern)) {
        return 10;
    }

    return static_cast<std::size_t>(std::stoull(match[1].str()));
}

std::size_t parseLimit(const std::string& query, std::size_t defaultLimit = 25, std::size_t maxLimit = 200) {
    const std::regex pattern("(^|&)limit=([0-9]+)(&|$)");
    std::smatch match;

    if (!std::regex_search(query, match, pattern)) {
        return defaultLimit;
    }

    const std::size_t limit = static_cast<std::size_t>(std::stoull(match[2].str()));
    return std::max<std::size_t>(1, std::min(limit, maxLimit));
}

std::optional<std::string> symbolFromPrefixedPath(const std::string& path, const std::string& prefix) {
    if (path.rfind(prefix, 0) != 0 || path.size() <= prefix.size()) {
        return std::nullopt;
    }

    return urlDecode(path.substr(prefix.size()));
}

HttpResponse jsonResponse(int status, const std::string& body) {
    return {status, "application/json", body};
}

HttpResponse orderResultResponse(AccountStore& accounts, const std::string& symbol, const SubmitResult& result) {
    accounts.recordTrades(symbol, result);
    return jsonResponse(200, serializeSubmitResult(result));
}

HttpResponse handleGetMe(Exchange& exchange, AccountStore& accounts, const std::string& path, const HttpRequest& request) {
    try {
        const TraderId traderId = authenticatedTraderId(request);

        if (path == "/me") {
            const std::vector<SymbolOpenOrder> orders = collectOpenOrders(exchange, traderId);
            const std::vector<FillRecord> fills = accounts.fillsForTrader(traderId);
            const std::vector<PositionRecord> positions = accounts.positionsForTrader(traderId);
            return jsonResponse(200, serializeMe(traderId, orders, fills, positions));
        }

        if (path == "/me/orders") {
            return jsonResponse(200, serializeOpenOrders(collectOpenOrders(exchange, traderId)));
        }

        if (path == "/me/fills" || path == "/me/trades") {
            return jsonResponse(200, serializeFills(accounts.fillsForTrader(traderId)));
        }

        if (path == "/me/positions") {
            return jsonResponse(200, serializePositions(accounts.positionsForTrader(traderId)));
        }

        if (path == "/me/portfolio") {
            return jsonResponse(200, serializePortfolio(accounts.portfolioForTrader(traderId)));
        }
    } catch (const AuthError& ex) {
        return jsonResponse(401, jsonError(ex.what()));
    } catch (const std::exception& ex) {
        return jsonResponse(400, jsonError(ex.what()));
    }

    return jsonResponse(404, jsonError("unknown GET endpoint"));
}

HttpResponse handlePostOrder(
    Exchange& exchange,
    AccountStore& accounts,
    OrderIdGenerator& orderIds,
    const std::string& path,
    const HttpRequest& request) {
    try {
        const TraderId traderId = authenticatedTraderId(request);
        const std::string& body = request.body;

        if (path == "/orders/buy") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            order.orderId = orderIds.next();
            return orderResultResponse(accounts, order.symbol,
                exchange.buy(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/sell") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            order.orderId = orderIds.next();
            return orderResultResponse(accounts, order.symbol,
                exchange.sell(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/market-buy") {
            ParsedOrder order = parseNewMarketOrder(body);
            order.orderId = orderIds.next();
            return orderResultResponse(accounts, order.symbol,
                exchange.marketBuy(order.symbol, traderId, order.orderId, order.quantity));
        }

        if (path == "/orders/market-sell") {
            ParsedOrder order = parseNewMarketOrder(body);
            order.orderId = orderIds.next();
            return orderResultResponse(accounts, order.symbol,
                exchange.marketSell(order.symbol, traderId, order.orderId, order.quantity));
        }

        if (path == "/orders/ioc-buy") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            order.orderId = orderIds.next();
            return orderResultResponse(accounts, order.symbol,
                exchange.iocBuy(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/ioc-sell") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            order.orderId = orderIds.next();
            return orderResultResponse(accounts, order.symbol,
                exchange.iocSell(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/fok-buy") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            order.orderId = orderIds.next();
            return orderResultResponse(accounts, order.symbol,
                exchange.fokBuy(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/fok-sell") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            order.orderId = orderIds.next();
            return orderResultResponse(accounts, order.symbol,
                exchange.fokSell(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/replace-buy") {
            const ParsedOrder order = parseOrderWithPrice(body);
            return orderResultResponse(accounts, order.symbol,
                exchange.replaceBuy(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/replace-sell") {
            const ParsedOrder order = parseOrderWithPrice(body);
            return orderResultResponse(accounts, order.symbol,
                exchange.replaceSell(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/cancel") {
            const auto symbol = jsonStringField(body, "symbol");
            const auto orderId = jsonIntField(body, "orderId");

            if (!symbol || !orderId) {
                throw std::runtime_error("expected symbol and orderId");
            }

            const bool canceled = exchange.cancelForTrader(*symbol, traderId, *orderId);
            return jsonResponse(200, std::string("{\"canceled\":") + (canceled ? "true" : "false") + "}");
        }
    } catch (const AuthError& ex) {
        return jsonResponse(401, jsonError(ex.what()));
    } catch (const std::exception& ex) {
        return jsonResponse(400, jsonError(ex.what()));
    }

    return jsonResponse(404, jsonError("unknown POST endpoint"));
}

HttpResponse route(Exchange& exchange, AccountStore& accounts, OrderIdGenerator& orderIds, const HttpRequest& request) {
    const auto [path, query] = splitQuery(request.path);

    if (request.method == "OPTIONS") {
        return jsonResponse(200, "{}");
    }

    if (request.method == "GET") {
        if (path == "/health") {
            return jsonResponse(200, "{\"ok\":true}");
        }

        if (path == "/symbols") {
            return jsonResponse(200, serializeSymbols(exchange.symbols()));
        }

        if (path == "/prices") {
            return jsonResponse(200, serializePrices(accounts.pricesForSymbols(exchange.symbols())));
        }

        const std::optional<std::string> priceSymbol = symbolFromPrefixedPath(path, "/prices/");
        if (priceSymbol) {
            return jsonResponse(200, serializePrice(accounts.priceForSymbol(*priceSymbol)));
        }

        const std::optional<std::string> tradesSymbol = symbolFromPrefixedPath(path, "/trades/");
        if (tradesSymbol) {
            return jsonResponse(200, serializeMarketTrades(accounts.tradesForSymbol(*tradesSymbol, parseLimit(query))));
        }

        const std::optional<std::string> symbol = symbolFromPrefixedPath(path, "/book/");
        if (symbol) {
            return jsonResponse(200, serializeSnapshot(*symbol, exchange.snapshot(*symbol, parseDepth(query))));
        }

        if (path == "/me" || path == "/me/orders" || path == "/me/fills" || path == "/me/trades"
            || path == "/me/positions" || path == "/me/portfolio") {
            return handleGetMe(exchange, accounts, path, request);
        }

        return jsonResponse(404, jsonError("unknown GET endpoint"));
    }

    if (request.method == "POST") {
        return handlePostOrder(exchange, accounts, orderIds, path, request);
    }

    return jsonResponse(405, jsonError("only GET, POST, and OPTIONS are supported"));
}

std::optional<HttpRequest> parseRequest(const std::string& raw, std::size_t headerEnd) {
    HttpRequest request;

    std::istringstream headerStream(raw.substr(0, headerEnd));
    std::string line;

    if (!std::getline(headerStream, line)) {
        return std::nullopt;
    }

    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream requestLine(line);
    requestLine >> request.method >> request.path;

    while (std::getline(headerStream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::size_t separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }

        request.headers[lower(trim(line.substr(0, separator)))] = trim(line.substr(separator + 1));
    }

    request.body = raw.substr(headerEnd + 4);
    return request;
}

std::optional<HttpRequest> readRequest(SocketHandle client) {
    std::string raw;
    char buffer[4096];

    while (raw.find("\r\n\r\n") == std::string::npos) {
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            return std::nullopt;
        }

        raw.append(buffer, static_cast<std::size_t>(received));
    }

    const std::size_t headerEnd = raw.find("\r\n\r\n");
    std::optional<HttpRequest> request = parseRequest(raw, headerEnd);
    if (!request) {
        return std::nullopt;
    }

    std::size_t contentLength = 0;
    const auto contentLengthHeader = request->headers.find("content-length");
    if (contentLengthHeader != request->headers.end()) {
        contentLength = static_cast<std::size_t>(std::stoull(contentLengthHeader->second));
    }

    const std::size_t bodyStart = headerEnd + 4;
    while (raw.size() < bodyStart + contentLength) {
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            return std::nullopt;
        }

        raw.append(buffer, static_cast<std::size_t>(received));
    }

    request->body = raw.substr(bodyStart, contentLength);
    return request;
}

void sendResponse(SocketHandle client, const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " " << statusText(response.status) << "\r\n"
        << "Content-Type: " << response.contentType << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        << "Content-Length: " << response.body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << response.body;

    const std::string payload = out.str();
    send(client, payload.c_str(), static_cast<int>(payload.size()), 0);
}

SocketHandle createServerSocket(int port) {
    SocketHandle server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == InvalidSocket) {
        throw std::runtime_error("failed to create socket");
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(port));

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(server);
        throw std::runtime_error("failed to bind socket");
    }

    if (listen(server, SOMAXCONN) != 0) {
        closeSocket(server);
        throw std::runtime_error("failed to listen on socket");
    }

    return server;
}

void seedDefaultBooks(Exchange& exchange) {
    exchange.ensureBook("BTC-USD");
    exchange.ensureBook("ETH-USD");
}

int parsePort(const char* value, int fallback) {
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }

    char* end = nullptr;
    const long port = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || port <= 0 || port > 65535) {
        throw std::runtime_error("invalid port");
    }

    return static_cast<int>(port);
}

}  // namespace

int main(int argc, char** argv) {
    int port = parsePort(std::getenv("PORT"), 8080);
    if (argc >= 2) {
        port = parsePort(argv[1], port);
    }

    try {
        SocketRuntime runtime;
        SocketHandle server = createServerSocket(port);
        Exchange exchange;
        AccountStore accounts;
        OrderIdGenerator orderIds;
        seedDefaultBooks(exchange);

        std::cout << "Orderbook API server listening on 0.0.0.0:" << port << "\n";
        std::cout << "Press Ctrl+C to stop.\n";

        while (true) {
            sockaddr_in clientAddress{};
            SocketLength clientAddressSize = sizeof(clientAddress);
            SocketHandle client = accept(server, reinterpret_cast<sockaddr*>(&clientAddress), &clientAddressSize);
            if (client == InvalidSocket) {
                continue;
            }

            std::optional<HttpRequest> request = readRequest(client);
            if (request) {
                sendResponse(client, route(exchange, accounts, orderIds, *request));
            } else {
                sendResponse(client, jsonResponse(400, jsonError("could not parse request")));
            }

            closeSocket(client);
        }

        closeSocket(server);
    } catch (const std::exception& ex) {
        std::cerr << "API server failed: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
