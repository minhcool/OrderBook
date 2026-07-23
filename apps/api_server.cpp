#include "orderbook/exchange.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef ORDERBOOK_WITH_POSTGRES
#include <libpq-fe.h>
#endif

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

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

std::int64_t epochSeconds(TimePoint value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

[[maybe_unused]] TimePoint timeFromEpochSeconds(std::int64_t value) {
    return TimePoint(std::chrono::seconds(value));
}

std::int64_t secondsRemaining(TimePoint deadline, TimePoint now = Clock::now()) {
    if (deadline <= now) {
        return 0;
    }

    return std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count();
}

std::int64_t envInt64(const char* name, std::int64_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }

    char* end = nullptr;
    const long long parsed = std::strtoll(value, &end, 10);
    if (end == value || *end != '\0') {
        throw std::runtime_error(std::string("invalid integer env var: ") + name);
    }

    return parsed;
}

struct GameConfig {
    std::int64_t startingCash = 100000;
    int startDelaySeconds = 90;
    int gameDurationSeconds = 600;
    int rejoinCooldownSeconds = 60;

    static GameConfig fromEnvironment() {
        GameConfig config;
        config.startingCash = envInt64("ORDERBOOK_STARTING_CASH", config.startingCash);
        config.startDelaySeconds = static_cast<int>(std::max<std::int64_t>(
            0,
            envInt64("ORDERBOOK_START_DELAY_SECONDS", config.startDelaySeconds)));
        config.gameDurationSeconds = static_cast<int>(std::max<std::int64_t>(
            60,
            envInt64("ORDERBOOK_GAME_DURATION_SECONDS", config.gameDurationSeconds)));
        config.rejoinCooldownSeconds = static_cast<int>(std::max<std::int64_t>(
            0,
            envInt64("ORDERBOOK_REJOIN_COOLDOWN_SECONDS", config.rejoinCooldownSeconds)));
        return config;
    }
};

class OrderIdGenerator {
public:
    OrderId next() {
        return nextId.fetch_add(1, std::memory_order_relaxed);
    }

    void setNextAtLeast(OrderId candidate) {
        OrderId current = nextId.load(std::memory_order_relaxed);
        while (current < candidate
            && !nextId.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
        }
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
    std::int64_t startingCash = 0;
    std::int64_t cashFlow = 0;
    std::int64_t cash = 0;
    std::int64_t reservedCash = 0;
    std::int64_t availableCash = 0;
    Qty reservedLongQuantity = 0;
    double marketValue = 0.0;
    double estimatedValue = 0.0;
    double tradingPnl = 0.0;
    double unrealizedPnl = 0.0;
    std::vector<PortfolioPositionRecord> positions;
};

struct SimulatorSymbolState {
    std::uint64_t tick = 0;
    double fairValue = 0.0;
};

struct SimulatorTickResult {
    int steps = 0;
    std::vector<std::string> symbols;
    std::size_t trades = 0;
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

    std::int64_t cashFlowForTrader(TraderId traderId) const {
        std::lock_guard<std::mutex> lock(accountMutex);
        return cashFlowForTraderLocked(traderId);
    }

    Qty positionQuantityForTrader(TraderId traderId, const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(accountMutex);
        return positionQuantityForTraderLocked(traderId, symbol);
    }

    PortfolioRecord portfolioForTrader(
        TraderId traderId,
        std::int64_t startingCash = 0,
        std::int64_t reservedCash = 0,
        Qty reservedLongQuantity = 0) const {
        std::lock_guard<std::mutex> lock(accountMutex);

        PortfolioRecord portfolio;
        portfolio.traderId = traderId;
        portfolio.startingCash = startingCash;
        portfolio.cashFlow = cashFlowForTraderLocked(traderId);
        portfolio.cash = portfolio.startingCash + portfolio.cashFlow;
        portfolio.reservedCash = reservedCash;
        portfolio.availableCash = portfolio.cash - portfolio.reservedCash;
        portfolio.reservedLongQuantity = reservedLongQuantity;
        const std::vector<PositionRecord> positions = positionsForTraderLocked(traderId);
        portfolio.positions.reserve(positions.size());

        for (const PositionRecord& position : positions) {
            PortfolioPositionRecord marked;
            marked.position = position;

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

        portfolio.tradingPnl = static_cast<double>(portfolio.cashFlow) + portfolio.marketValue;
        portfolio.estimatedValue = static_cast<double>(portfolio.cash) + portfolio.marketValue;
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

    std::int64_t cashFlowForTraderLocked(TraderId traderId) const {
        std::int64_t cashFlow = 0;
        for (const FillRecord& fill : fills) {
            if (fill.traderId != traderId) {
                continue;
            }

            cashFlow += fill.side == Side::Buy ? -fill.notional : fill.notional;
        }

        return cashFlow;
    }

    Qty positionQuantityForTraderLocked(TraderId traderId, const std::string& symbol) const {
        Qty quantity = 0;
        for (const FillRecord& fill : fills) {
            if (fill.traderId != traderId || fill.symbol != symbol) {
                continue;
            }

            quantity += fill.side == Side::Buy ? fill.quantity : -fill.quantity;
        }

        return quantity;
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

enum class RoomMode {
    Single,
    Competitive,
};

enum class ParticipantTrack {
    Manual,
    Bot,
};

enum class LobbyPhase {
    Waiting,
    Starting,
    Running,
    Finished,
};

struct AssetConfig {
    std::string symbol;
    std::string displayName;
    std::string behavior;
    std::string source;
    Price referencePrice = 0;
    std::string signalQuality;
};

struct GameRoom {
    std::string id;
    std::string name;
    RoomMode mode = RoomMode::Single;
    std::string difficulty;
    std::int64_t startingCash = 100000;
    int maxParticipants = 1;
    int startDelaySeconds = 0;
    int gameDurationSeconds = 600;
    int rejoinCooldownSeconds = 60;
    bool houseLiquidity = true;
    Exchange exchange;
    AccountStore accounts;
    OrderIdGenerator orderIds;
    std::vector<AssetConfig> assets;
    std::map<std::string, SimulatorSymbolState> simulator;
};

struct ParsedRoomPath {
    std::string roomId;
    std::string nestedPath;
};

enum class LobbyJoinResult {
    Joined,
    AlreadyJoined,
    Full,
    ActiveElsewhere,
    CoolingDown,
    GameClosed,
};

struct LobbyMembershipRecord {
    TraderId traderId = 0;
    ParticipantTrack track = ParticipantTrack::Manual;
    TimePoint joinedAt{};
};

struct RatingRecord {
    TraderId traderId = 0;
    ParticipantTrack track = ParticipantTrack::Manual;
    int ratingBefore = 1200;
    int ratingAfter = 1200;
    double score = 0.0;
};

struct LeaderboardRow {
    TraderId traderId = 0;
    ParticipantTrack track = ParticipantTrack::Manual;
    double pnl = 0.0;
    double estimatedValue = 0.0;
    int ratingBefore = 1200;
    int ratingAfter = 1200;
};

struct LobbyJoinOutcome {
    LobbyJoinResult result = LobbyJoinResult::Joined;
    std::int64_t cooldownRemainingSeconds = 0;
    std::string activeLobbyId;
};

LobbyJoinOutcome makeJoinOutcome(
    LobbyJoinResult result,
    std::int64_t cooldownRemainingSeconds = 0,
    const std::string& activeLobbyId = "") {
    return {result, cooldownRemainingSeconds, activeLobbyId};
}

struct ActiveSession {
    std::string id;
    std::string roomId;
    bool competitive = false;
};

struct PersistedEvent {
    std::int64_t id = 0;
    std::string eventType;
    std::string sessionId;
    std::string roomId;
    RoomMode roomMode = RoomMode::Single;
    TraderId traderId = 0;
    ParticipantTrack track = ParticipantTrack::Manual;
    std::string symbol;
    Side side = Side::Buy;
    std::string orderMode;
    OrderId orderId = 0;
    Price price = 0;
    Qty quantity = 0;
    bool accepted = false;
    bool canceled = false;
    std::optional<TimePoint> cooldownUntil;
    TimePoint createdAt{};
};

struct RestoreStats {
    std::size_t events = 0;
    std::size_t sessions = 0;
    std::size_t orders = 0;
    std::size_t cancels = 0;
    std::size_t simulatorTicks = 0;
    std::size_t skipped = 0;
};

struct CompetitiveLobby {
    std::string id;
    std::string name;
    std::string roomId;
    int capacity = 0;
    int startDelaySeconds = 90;
    int gameDurationSeconds = 600;
    std::unique_ptr<GameRoom> game;

    LobbyJoinResult join(TraderId traderId, ParticipantTrack track) {
        return joinAt(traderId, track, Clock::now());
    }

    LobbyJoinResult joinAt(TraderId traderId, ParticipantTrack track, TimePoint now) {
        std::lock_guard<std::mutex> lock(participantMutex);
        updatePhaseLocked(now);

        if (participants.contains(traderId)) {
            return LobbyJoinResult::AlreadyJoined;
        }

        const auto admitted = admittedParticipants.find(traderId);
        const bool returningParticipant = admitted != admittedParticipants.end();
        if (phase == LobbyPhase::Finished
            || (phase == LobbyPhase::Running && !returningParticipant)) {
            return LobbyJoinResult::GameClosed;
        }

        if (participants.size() >= static_cast<std::size_t>(capacity)) {
            return LobbyJoinResult::Full;
        }

        const ParticipantTrack admittedTrack = returningParticipant ? admitted->second.track : track;
        const LobbyMembershipRecord membership{traderId, admittedTrack, now};
        participants[traderId] = membership;
        admittedParticipants.emplace(traderId, membership);
        maybeScheduleStartLocked(now);
        return LobbyJoinResult::Joined;
    }

    bool leave(TraderId traderId) {
        return leaveAt(traderId, Clock::now());
    }

    bool leaveAt(TraderId traderId, TimePoint now) {
        std::lock_guard<std::mutex> lock(participantMutex);
        updatePhaseLocked(now);
        const bool left = participants.erase(traderId) > 0;

        if (participants.empty() && (phase == LobbyPhase::Waiting || phase == LobbyPhase::Starting)) {
            phase = LobbyPhase::Waiting;
            startsAt.reset();
            endsAt.reset();
        } else if (phase == LobbyPhase::Starting && participants.size() < static_cast<std::size_t>(minStartParticipants())) {
            phase = LobbyPhase::Waiting;
            startsAt.reset();
            endsAt.reset();
        }

        return left;
    }

    bool contains(TraderId traderId) const {
        std::lock_guard<std::mutex> lock(participantMutex);
        return participants.contains(traderId);
    }

    bool wasAdmitted(TraderId traderId) const {
        std::lock_guard<std::mutex> lock(participantMutex);
        return admittedParticipants.contains(traderId);
    }

    std::optional<LobbyMembershipRecord> membershipFor(TraderId traderId) const {
        std::lock_guard<std::mutex> lock(participantMutex);
        const auto found = participants.find(traderId);
        if (found == participants.end()) {
            return std::nullopt;
        }

        return found->second;
    }

    std::size_t participantCount() const {
        std::lock_guard<std::mutex> lock(participantMutex);
        return participants.size();
    }

    int minStartParticipants() const {
        return std::max(1, (capacity + 1) / 2);
    }

    LobbyPhase currentPhase() const {
        std::lock_guard<std::mutex> lock(participantMutex);
        updatePhaseLocked(Clock::now());
        return phase;
    }

    std::optional<TimePoint> currentStartsAt() const {
        std::lock_guard<std::mutex> lock(participantMutex);
        updatePhaseLocked(Clock::now());
        return startsAt;
    }

    std::optional<TimePoint> currentEndsAt() const {
        std::lock_guard<std::mutex> lock(participantMutex);
        updatePhaseLocked(Clock::now());
        return endsAt;
    }

    std::int64_t startsInSeconds() const {
        std::lock_guard<std::mutex> lock(participantMutex);
        updatePhaseLocked(Clock::now());
        return startsAt ? secondsRemaining(*startsAt) : 0;
    }

    std::int64_t endsInSeconds() const {
        std::lock_guard<std::mutex> lock(participantMutex);
        updatePhaseLocked(Clock::now());
        return endsAt ? secondsRemaining(*endsAt) : 0;
    }

    std::vector<LobbyMembershipRecord> participantRecords() const {
        std::lock_guard<std::mutex> lock(participantMutex);
        std::vector<LobbyMembershipRecord> result;
        result.reserve(participants.size());
        for (const auto& [traderId, membership] : participants) {
            (void)traderId;
            result.push_back(membership);
        }

        return result;
    }

    void finalizeIfNeeded(const std::unordered_map<TraderId, int>& manualRatings,
        const std::unordered_map<TraderId, int>& botRatings) {
        std::lock_guard<std::mutex> lock(participantMutex);
        updatePhaseLocked(Clock::now());
        if (phase != LobbyPhase::Finished || ratingsFinalized) {
            return;
        }

        leaderboard.clear();
        std::map<ParticipantTrack, std::vector<LeaderboardRow>> rowsByTrack;
        for (const auto& [traderId, membership] : admittedParticipants) {
            const PortfolioRecord portfolio = game->accounts.portfolioForTrader(
                traderId,
                game->startingCash,
                reservedCashForTrader(*game, traderId),
                reservedSellQuantityForTrader(*game, traderId));

            const auto& ratingBook = membership.track == ParticipantTrack::Bot ? botRatings : manualRatings;
            const auto foundRating = ratingBook.find(traderId);
            const int ratingBefore = foundRating == ratingBook.end() ? 1200 : foundRating->second;

            rowsByTrack[membership.track].push_back({
                traderId,
                membership.track,
                portfolio.tradingPnl,
                portfolio.estimatedValue,
                ratingBefore,
                ratingBefore,
            });
        }

        for (auto& [track, rows] : rowsByTrack) {
            (void)track;
            applyElo(rows);
            leaderboard.insert(leaderboard.end(), rows.begin(), rows.end());
        }

        std::sort(leaderboard.begin(), leaderboard.end(), [](const LeaderboardRow& left, const LeaderboardRow& right) {
            if (left.track != right.track) {
                return static_cast<int>(left.track) < static_cast<int>(right.track);
            }

            return left.estimatedValue > right.estimatedValue;
        });

        ratingsFinalized = true;
    }

    bool hasFinalRatings() const {
        std::lock_guard<std::mutex> lock(participantMutex);
        return ratingsFinalized;
    }

    std::vector<LeaderboardRow> leaderboardRows() const {
        std::lock_guard<std::mutex> lock(participantMutex);
        return leaderboard;
    }

private:
    void maybeScheduleStartLocked(TimePoint now) {
        if (phase != LobbyPhase::Waiting || participants.size() < static_cast<std::size_t>(minStartParticipants())) {
            return;
        }

        startsAt = now + std::chrono::seconds(startDelaySeconds);
        endsAt = *startsAt + std::chrono::seconds(gameDurationSeconds);
        phase = startDelaySeconds == 0 ? LobbyPhase::Running : LobbyPhase::Starting;
    }

    void updatePhaseLocked(TimePoint now) const {
        if (phase == LobbyPhase::Finished) {
            return;
        }

        if (startsAt && now >= *startsAt && phase == LobbyPhase::Starting) {
            phase = LobbyPhase::Running;
        }

        if (endsAt && now >= *endsAt) {
            phase = LobbyPhase::Finished;
        }
    }

    static double expectedScore(int leftRating, int rightRating) {
        return 1.0 / (1.0 + std::pow(10.0, static_cast<double>(rightRating - leftRating) / 400.0));
    }

    static void applyElo(std::vector<LeaderboardRow>& rows) {
        constexpr double K = 32.0;
        if (rows.size() < 2) {
            return;
        }

        std::vector<double> deltas(rows.size(), 0.0);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            for (std::size_t j = i + 1; j < rows.size(); ++j) {
                const double actualI =
                    rows[i].estimatedValue > rows[j].estimatedValue ? 1.0
                    : rows[i].estimatedValue < rows[j].estimatedValue ? 0.0
                    : 0.5;
                const double expectedI = expectedScore(rows[i].ratingBefore, rows[j].ratingBefore);
                deltas[i] += K * (actualI - expectedI);
                deltas[j] += K * ((1.0 - actualI) - (1.0 - expectedI));
            }
        }

        for (std::size_t i = 0; i < rows.size(); ++i) {
            rows[i].ratingAfter = static_cast<int>(std::llround(rows[i].ratingBefore + deltas[i]));
        }
    }

    static std::int64_t reservedCashForTrader(const GameRoom& room, TraderId traderId);
    static Qty reservedSellQuantityForTrader(const GameRoom& room, TraderId traderId);

    mutable std::mutex participantMutex;
    std::map<TraderId, LobbyMembershipRecord> participants;
    std::map<TraderId, LobbyMembershipRecord> admittedParticipants;
    mutable LobbyPhase phase = LobbyPhase::Waiting;
    mutable std::optional<TimePoint> startsAt;
    mutable std::optional<TimePoint> endsAt;
    bool ratingsFinalized = false;
    std::vector<LeaderboardRow> leaderboard;
};

struct ParsedLobbyPath {
    std::string lobbyId;
    std::string nestedPath;
};

struct SinglePlayerSession {
    std::string id;
    std::string roomId;
    TraderId traderId = 0;
    std::unique_ptr<GameRoom> game;
    TimePoint joinedAt{};
    bool active = true;
};

class RoomStore {
public:
    explicit RoomStore(GameConfig config);

    GameRoom* find(const std::string& roomId);
    CompetitiveLobby* findLobby(const std::string& lobbyId);
    SinglePlayerSession* findSingleSession(const std::string& roomId, TraderId traderId);
    std::vector<const GameRoom*> rooms() const;
    std::vector<const CompetitiveLobby*> lobbies() const;
    std::vector<const CompetitiveLobby*> lobbiesForRoom(const std::string& roomId) const;
    LobbyJoinOutcome joinSingleRoom(const std::string& roomId, TraderId traderId);
    bool leaveSingleRoom(const std::string& roomId, TraderId traderId);
    LobbyJoinOutcome joinCompetitiveLobby(const std::string& lobbyId, TraderId traderId, ParticipantTrack track);
    bool leaveCompetitiveLobby(const std::string& lobbyId, TraderId traderId);
    bool hasActiveSession(TraderId traderId, const std::string& sessionId) const;
    std::optional<ActiveSession> activeSessionFor(TraderId traderId) const;
    std::int64_t cooldownRemainingSeconds(TraderId traderId) const;
    const GameConfig& config() const;
    std::unordered_map<TraderId, int>& ratings(ParticipantTrack track);
    bool storeLeaderboardRatings(const CompetitiveLobby& lobby);
    bool restoreSessionJoin(const PersistedEvent& event);
    bool restoreSessionLeave(const PersistedEvent& event);
    GameRoom* gameForPersistedSession(const PersistedEvent& event);

private:
    void addRoom(std::unique_ptr<GameRoom> room);
    void addLobby(std::unique_ptr<CompetitiveLobby> lobby);
    std::unique_ptr<GameRoom> cloneSingleRoom(const GameRoom& room) const;
    SinglePlayerSession* ensureSingleSessionLocked(const std::string& roomId, TraderId traderId, TimePoint joinedAt);
    LobbyJoinOutcome canJoinSessionLocked(TraderId traderId, const std::string& sessionId) const;
    void markJoinedLocked(TraderId traderId, const ActiveSession& session);
    void markLeftLocked(TraderId traderId);
    void markLeftLocked(TraderId traderId, TimePoint cooldownUntil);

    GameConfig gameConfig;
    mutable std::mutex sessionMutex;
    std::map<std::string, std::unique_ptr<GameRoom>> roomById;
    std::map<std::string, std::unique_ptr<CompetitiveLobby>> lobbyById;
    std::map<std::string, std::map<TraderId, std::unique_ptr<SinglePlayerSession>>> singleSessionsByRoom;
    std::map<TraderId, ActiveSession> activeByTrader;
    std::map<TraderId, TimePoint> cooldownUntilByTrader;
    std::unordered_map<TraderId, int> manualRatings;
    std::unordered_map<TraderId, int> botRatings;
    std::set<std::string> storedRatingLobbies;
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
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 409:
            return "Conflict";
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

std::string roomModeToString(RoomMode mode) {
    return mode == RoomMode::Single ? "single" : "competitive";
}

[[maybe_unused]] RoomMode parseRoomMode(const std::string& value) {
    return lower(value) == "competitive" ? RoomMode::Competitive : RoomMode::Single;
}

std::string participantTrackToString(ParticipantTrack track) {
    return track == ParticipantTrack::Bot ? "bot" : "manual";
}

ParticipantTrack parseParticipantTrack(const std::string& value) {
    return lower(value) == "bot" ? ParticipantTrack::Bot : ParticipantTrack::Manual;
}

[[maybe_unused]] Side parseSide(const std::string& value) {
    return lower(value) == "sell" ? Side::Sell : Side::Buy;
}

std::string lobbyPhaseToString(LobbyPhase phase) {
    switch (phase) {
        case LobbyPhase::Waiting:
            return "waiting";
        case LobbyPhase::Starting:
            return "starting";
        case LobbyPhase::Running:
            return "running";
        case LobbyPhase::Finished:
            return "finished";
    }

    return "waiting";
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

std::string serializeSimulatorTickResult(const SimulatorTickResult& result) {
    std::ostringstream out;
    out << "{\"advanced\":true,"
        << "\"steps\":" << result.steps << ","
        << "\"trades\":" << result.trades << ","
        << "\"symbols\":[";

    for (std::size_t i = 0; i < result.symbols.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        out << "\"" << jsonEscape(result.symbols[i]) << "\"";
    }

    out << "]}";
    return out.str();
}

std::string serializeAsset(const AssetConfig& asset) {
    std::ostringstream out;
    out << "{"
        << "\"symbol\":\"" << jsonEscape(asset.symbol) << "\","
        << "\"displayName\":\"" << jsonEscape(asset.displayName) << "\","
        << "\"behavior\":\"" << jsonEscape(asset.behavior) << "\","
        << "\"source\":\"" << jsonEscape(asset.source) << "\","
        << "\"referencePrice\":" << asset.referencePrice << ","
        << "\"signalQuality\":\"" << jsonEscape(asset.signalQuality) << "\""
        << "}";
    return out.str();
}

std::string serializeAssets(const std::vector<AssetConfig>& assets) {
    std::ostringstream out;
    out << "[";

    for (std::size_t i = 0; i < assets.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        out << serializeAsset(assets[i]);
    }

    out << "]";
    return out.str();
}

std::string serializeRoom(const GameRoom& room, bool includeAssets = true) {
    std::ostringstream out;
    out << "{"
        << "\"id\":\"" << jsonEscape(room.id) << "\","
        << "\"name\":\"" << jsonEscape(room.name) << "\","
        << "\"mode\":\"" << roomModeToString(room.mode) << "\","
        << "\"difficulty\":\"" << jsonEscape(room.difficulty) << "\","
        << "\"startingCash\":" << room.startingCash << ","
        << "\"maxParticipants\":" << room.maxParticipants << ","
        << "\"startDelaySeconds\":" << room.startDelaySeconds << ","
        << "\"gameDurationSeconds\":" << room.gameDurationSeconds << ","
        << "\"rejoinCooldownSeconds\":" << room.rejoinCooldownSeconds << ","
        << "\"houseLiquidity\":" << (room.houseLiquidity ? "true" : "false") << ","
        << "\"assets\":" << (includeAssets ? serializeAssets(room.assets) : "[]")
        << "}";
    return out.str();
}

std::string serializeRooms(const std::vector<const GameRoom*>& rooms, bool includeAssets = true) {
    std::ostringstream out;
    out << "{\"rooms\":[";

    for (std::size_t i = 0; i < rooms.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        out << serializeRoom(*rooms[i], includeAssets);
    }

    out << "]}";
    return out.str();
}

std::string serializeLobby(const CompetitiveLobby& lobby) {
    const std::size_t participantCount = lobby.participantCount();
    const std::size_t capacity = static_cast<std::size_t>(lobby.capacity);
    const std::size_t spotsRemaining = capacity > participantCount ? capacity - participantCount : 0;
    const LobbyPhase phase = lobby.currentPhase();
    const std::optional<TimePoint> startsAt = lobby.currentStartsAt();
    const std::optional<TimePoint> endsAt = lobby.currentEndsAt();
    const bool acceptingJoins = phase == LobbyPhase::Waiting || phase == LobbyPhase::Starting;
    const std::string status = !acceptingJoins ? "closed" : (spotsRemaining == 0 ? "full" : "open");

    std::ostringstream out;
    out << "{"
        << "\"id\":\"" << jsonEscape(lobby.id) << "\","
        << "\"name\":\"" << jsonEscape(lobby.name) << "\","
        << "\"roomId\":\"" << jsonEscape(lobby.roomId) << "\","
        << "\"status\":\"" << status << "\","
        << "\"phase\":\"" << lobbyPhaseToString(phase) << "\","
        << "\"participantCount\":" << participantCount << ","
        << "\"capacity\":" << lobby.capacity << ","
        << "\"spotsRemaining\":" << (acceptingJoins ? spotsRemaining : 0) << ","
        << "\"minStartParticipants\":" << lobby.minStartParticipants() << ","
        << "\"startDelaySeconds\":" << lobby.startDelaySeconds << ","
        << "\"gameDurationSeconds\":" << lobby.gameDurationSeconds << ","
        << "\"startsAt\":" << (startsAt ? std::to_string(epochSeconds(*startsAt)) : "null") << ","
        << "\"endsAt\":" << (endsAt ? std::to_string(epochSeconds(*endsAt)) : "null") << ","
        << "\"startsInSeconds\":" << lobby.startsInSeconds() << ","
        << "\"endsInSeconds\":" << lobby.endsInSeconds()
        << "}";
    return out.str();
}

std::string serializeLobbies(const std::vector<const CompetitiveLobby*>& lobbies) {
    std::ostringstream out;
    out << "{\"lobbies\":[";

    for (std::size_t i = 0; i < lobbies.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        out << serializeLobby(*lobbies[i]);
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

std::int64_t reservedBuyCashForTrader(
    const Exchange& exchange,
    TraderId traderId,
    std::optional<OrderId> excludeOrderId = std::nullopt) {
    std::int64_t reserved = 0;
    for (const SymbolOpenOrder& item : collectOpenOrders(exchange, traderId)) {
        if (excludeOrderId && item.order.orderId == *excludeOrderId) {
            continue;
        }

        if (item.order.side == Side::Buy) {
            reserved += item.order.price * item.order.remainingQuantity;
        }
    }

    return reserved;
}

Qty reservedOpenSellQuantityForTrader(
    const Exchange& exchange,
    TraderId traderId,
    const std::string& symbol,
    std::optional<OrderId> excludeOrderId = std::nullopt) {
    Qty reserved = 0;
    for (const OpenOrder& order : exchange.openOrders(symbol, traderId)) {
        if (excludeOrderId && order.orderId == *excludeOrderId) {
            continue;
        }

        if (order.side == Side::Sell) {
            reserved += order.remainingQuantity;
        }
    }

    return reserved;
}

std::int64_t CompetitiveLobby::reservedCashForTrader(const GameRoom& room, TraderId traderId) {
    return reservedBuyCashForTrader(room.exchange, traderId);
}

Qty CompetitiveLobby::reservedSellQuantityForTrader(const GameRoom& room, TraderId traderId) {
    Qty total = 0;
    for (const std::string& symbol : room.exchange.symbols()) {
        total += reservedOpenSellQuantityForTrader(room.exchange, traderId, symbol);
    }

    return total;
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
        << "\"startingCash\":" << portfolio.startingCash << ","
        << "\"cashFlow\":" << portfolio.cashFlow << ","
        << "\"cash\":" << portfolio.cash << ","
        << "\"reservedCash\":" << portfolio.reservedCash << ","
        << "\"availableCash\":" << portfolio.availableCash << ","
        << "\"reservedLongQuantity\":" << portfolio.reservedLongQuantity << ","
        << "\"marketValue\":" << jsonDouble(portfolio.marketValue) << ","
        << "\"estimatedValue\":" << jsonDouble(portfolio.estimatedValue) << ","
        << "\"tradingPnl\":" << jsonDouble(portfolio.tradingPnl) << ","
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

std::string serializeLeaderboard(const std::vector<LeaderboardRow>& rows) {
    std::ostringstream out;
    out << "{\"leaderboard\":[";

    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) {
            out << ",";
        }

        const LeaderboardRow& row = rows[i];
        out << "{"
            << "\"rank\":" << (i + 1) << ","
            << "\"traderId\":" << row.traderId << ","
            << "\"track\":\"" << participantTrackToString(row.track) << "\","
            << "\"pnl\":" << jsonDouble(row.pnl) << ","
            << "\"estimatedValue\":" << jsonDouble(row.estimatedValue) << ","
            << "\"ratingBefore\":" << row.ratingBefore << ","
            << "\"ratingAfter\":" << row.ratingAfter
            << "}";
    }

    out << "]}";
    return out.str();
}

std::string serializeActiveSession(const std::optional<ActiveSession>& session) {
    if (!session) {
        return "null";
    }

    std::ostringstream out;
    out << "{"
        << "\"id\":\"" << jsonEscape(session->id) << "\","
        << "\"roomId\":\"" << jsonEscape(session->roomId) << "\","
        << "\"competitive\":" << (session->competitive ? "true" : "false")
        << "}";
    return out.str();
}

std::string joinResultMessage(LobbyJoinResult result) {
    switch (result) {
        case LobbyJoinResult::Joined:
            return "joined";
        case LobbyJoinResult::AlreadyJoined:
            return "already joined";
        case LobbyJoinResult::Full:
            return "lobby is full";
        case LobbyJoinResult::ActiveElsewhere:
            return "leave your current lobby before joining another";
        case LobbyJoinResult::CoolingDown:
            return "wait for the rejoin cooldown";
        case LobbyJoinResult::GameClosed:
            return "this game is closed";
    }

    return "join failed";
}

std::string serializeJoinOutcome(
    const LobbyJoinOutcome& outcome,
    TraderId traderId,
    const std::optional<ActiveSession>& activeSession,
    const std::string& targetJson) {
    std::ostringstream out;
    const bool joined = outcome.result == LobbyJoinResult::Joined || outcome.result == LobbyJoinResult::AlreadyJoined;
    out << "{"
        << "\"joined\":" << (joined ? "true" : "false") << ","
        << "\"alreadyJoined\":" << (outcome.result == LobbyJoinResult::AlreadyJoined ? "true" : "false") << ","
        << "\"traderId\":" << traderId << ","
        << "\"message\":\"" << jsonEscape(joinResultMessage(outcome.result)) << "\","
        << "\"cooldownRemainingSeconds\":" << outcome.cooldownRemainingSeconds << ","
        << "\"activeSession\":" << serializeActiveSession(activeSession);
    if (!targetJson.empty()) {
        out << "," << targetJson;
    }

    out << "}";
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

struct AuthenticatedUser {
    TraderId traderId = 0;
    std::string subject;
};

AuthenticatedUser authenticatedUser(const HttpRequest& request) {
    const std::optional<std::string> token = bearerToken(request);
    if (!token) {
        throw AuthError("missing Authorization bearer token");
    }

    const std::optional<std::string> subject = jwtSubjectWithoutVerification(*token);
    if (!subject || subject->empty()) {
        throw AuthError("bearer token must be a Clerk JWT with a sub claim");
    }

    return {traderIdFromSubject(*subject), *subject};
}

TraderId authenticatedTraderId(const HttpRequest& request) {
    return authenticatedUser(request).traderId;
}

class PersistenceStore {
public:
    PersistenceStore() {
#ifdef ORDERBOOK_WITH_POSTGRES
        checkpointEveryEvents = static_cast<int>(std::max<std::int64_t>(
            1,
            envInt64("ORDERBOOK_CHECKPOINT_EVERY_EVENTS", checkpointEveryEvents)));

        const char* databaseUrl = std::getenv("DATABASE_URL");
        if (databaseUrl == nullptr || databaseUrl[0] == '\0') {
            return;
        }

        connection = PQconnectdb(databaseUrl);
        if (PQstatus(connection) != CONNECTION_OK) {
            std::cerr << "PostgreSQL disabled: " << PQerrorMessage(connection) << "\n";
            PQfinish(connection);
            connection = nullptr;
            return;
        }

        ensureSchema();
        std::cout << "PostgreSQL persistence enabled.\n";
#endif
    }

    std::vector<PersistedEvent> loadReplayEvents() const {
        std::vector<PersistedEvent> events;
#ifdef ORDERBOOK_WITH_POSTGRES
        if (connection == nullptr) {
            return events;
        }

        PGresult* query = PQexec(
            connection,
            "SELECT id, event_type, session_id, room_id, room_mode, trader_id, track, symbol, side, "
            "order_mode, order_id, price, quantity, accepted, canceled, cooldown_until_epoch, created_at_epoch "
            "FROM orderbook_events ORDER BY id");
        if (PQresultStatus(query) != PGRES_TUPLES_OK) {
            std::cerr << "PostgreSQL event replay load failed: " << PQerrorMessage(connection) << "\n";
            PQclear(query);
            return events;
        }

        const auto cell = [query](int row, int col) -> std::string {
            return PQgetisnull(query, row, col) ? "" : PQgetvalue(query, row, col);
        };
        const auto boolCell = [&cell](int row, int col) {
            const std::string value = lower(cell(row, col));
            return value == "t" || value == "true" || value == "1";
        };
        const auto intCell = [&cell](int row, int col) -> std::int64_t {
            const std::string value = cell(row, col);
            return value.empty() ? 0 : std::stoll(value);
        };

        const int rows = PQntuples(query);
        events.reserve(static_cast<std::size_t>(rows));
        for (int row = 0; row < rows; ++row) {
            PersistedEvent event;
            event.id = intCell(row, 0);
            event.eventType = cell(row, 1);
            event.sessionId = cell(row, 2);
            event.roomId = cell(row, 3);
            event.roomMode = parseRoomMode(cell(row, 4));
            event.traderId = intCell(row, 5);
            event.track = parseParticipantTrack(cell(row, 6));
            event.symbol = cell(row, 7);
            event.side = parseSide(cell(row, 8));
            event.orderMode = cell(row, 9);
            event.orderId = intCell(row, 10);
            event.price = intCell(row, 11);
            event.quantity = intCell(row, 12);
            event.accepted = boolCell(row, 13);
            event.canceled = boolCell(row, 14);
            if (!cell(row, 15).empty()) {
                event.cooldownUntil = timeFromEpochSeconds(intCell(row, 15));
            }
            event.createdAt = timeFromEpochSeconds(intCell(row, 16));
            events.push_back(std::move(event));
        }

        PQclear(query);
#endif
        return events;
    }

    ~PersistenceStore() {
#ifdef ORDERBOOK_WITH_POSTGRES
        if (connection != nullptr) {
            PQfinish(connection);
        }
#endif
    }

    bool enabled() const {
#ifdef ORDERBOOK_WITH_POSTGRES
        return connection != nullptr;
#else
        return false;
#endif
    }

    std::unordered_map<TraderId, int> loadRatings(ParticipantTrack track) const {
        (void)track;
        std::unordered_map<TraderId, int> result;
#ifdef ORDERBOOK_WITH_POSTGRES
        if (connection == nullptr) {
            return result;
        }

        const char* column = track == ParticipantTrack::Bot ? "bot_elo" : "manual_elo";
        const std::string sql = std::string("SELECT trader_id, ") + column + " FROM orderbook_users";
        PGresult* query = PQexec(connection, sql.c_str());
        if (PQresultStatus(query) != PGRES_TUPLES_OK) {
            std::cerr << "PostgreSQL rating load failed: " << PQerrorMessage(connection) << "\n";
            PQclear(query);
            return result;
        }

        const int rows = PQntuples(query);
        for (int row = 0; row < rows; ++row) {
            result[std::stoll(PQgetvalue(query, row, 0))] = std::stoi(PQgetvalue(query, row, 1));
        }

        PQclear(query);
#endif
        return result;
    }

    void upsertUser(const AuthenticatedUser& user) {
        execParams(
            "INSERT INTO orderbook_users (trader_id, clerk_subject) VALUES ($1, $2) "
            "ON CONFLICT (trader_id) DO UPDATE SET clerk_subject = EXCLUDED.clerk_subject, updated_at = now()",
            {std::to_string(user.traderId), user.subject});
    }

    void recordSessionEvent(
        const std::string& sessionId,
        const std::string& roomId,
        RoomMode mode,
        TraderId traderId,
        ParticipantTrack track,
        const std::string& eventType,
        std::optional<TimePoint> cooldownUntil = std::nullopt) {
        PersistedEvent event;
        event.eventType = eventType == "leave" ? "session_leave" : "session_join";
        event.sessionId = sessionId;
        event.roomId = roomId;
        event.roomMode = mode;
        event.traderId = traderId;
        event.track = track;
        event.cooldownUntil = cooldownUntil;
        event.createdAt = Clock::now();
        appendPersistedEvent(event);

        execParams(
            "INSERT INTO orderbook_session_events "
            "(session_id, room_id, mode, trader_id, track, event_type, cooldown_until_epoch) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7)",
            {
                sessionId,
                roomId,
                roomModeToString(mode),
                std::to_string(traderId),
                participantTrackToString(track),
                eventType,
                cooldownUntil ? std::to_string(epochSeconds(*cooldownUntil)) : "",
            });
    }

    void recordOrder(
        const std::string& sessionId,
        const std::string& roomId,
        RoomMode roomMode,
        TraderId traderId,
        Side side,
        const std::string& mode,
        const ParsedOrder& order,
        const SubmitResult& result) {
        PersistedEvent event;
        event.eventType = "order_submit";
        event.sessionId = sessionId;
        event.roomId = roomId;
        event.roomMode = roomMode;
        event.traderId = traderId;
        event.symbol = order.symbol;
        event.side = side;
        event.orderMode = mode;
        event.orderId = order.orderId;
        event.price = order.price;
        event.quantity = order.quantity;
        event.accepted = result.accepted;
        event.createdAt = Clock::now();
        appendPersistedEvent(event);

        execParams(
            "INSERT INTO orderbook_orders "
            "(session_id, trader_id, order_id, symbol, side, mode, price, quantity, accepted, "
            "filled_quantity, resting_quantity, notional, message) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13)",
            {
                sessionId,
                std::to_string(traderId),
                std::to_string(order.orderId),
                order.symbol,
                sideToString(side),
                mode,
                std::to_string(order.price),
                std::to_string(order.quantity),
                result.accepted ? "true" : "false",
                std::to_string(result.filledQuantity),
                std::to_string(result.restingQuantity),
                std::to_string(result.notional),
                result.message,
            });
    }

    void recordCancel(
        const std::string& sessionId,
        const std::string& roomId,
        RoomMode roomMode,
        TraderId traderId,
        const std::string& symbol,
        OrderId orderId,
        bool canceled) {
        PersistedEvent event;
        event.eventType = "order_cancel";
        event.sessionId = sessionId;
        event.roomId = roomId;
        event.roomMode = roomMode;
        event.traderId = traderId;
        event.symbol = symbol;
        event.orderMode = "cancel";
        event.orderId = orderId;
        event.canceled = canceled;
        event.createdAt = Clock::now();
        appendPersistedEvent(event);

        execParams(
            "INSERT INTO orderbook_cancels "
            "(session_id, trader_id, symbol, order_id, canceled) "
            "VALUES ($1, $2, $3, $4, $5)",
            {
                sessionId,
                std::to_string(traderId),
                symbol,
                std::to_string(orderId),
                canceled ? "true" : "false",
            });
    }

    void recordSimulatorTick(
        const std::string& sessionId,
        const std::string& roomId,
        TraderId traderId,
        int steps) {
        PersistedEvent event;
        event.eventType = "simulator_tick";
        event.sessionId = sessionId;
        event.roomId = roomId;
        event.roomMode = RoomMode::Single;
        event.traderId = traderId;
        event.orderMode = "simulator";
        event.quantity = steps;
        event.createdAt = Clock::now();
        appendPersistedEvent(event);
    }

    void recordTrades(const std::string& sessionId, const std::string& symbol, const SubmitResult& result) {
        for (const Trade& trade : result.trades) {
            execParams(
                "INSERT INTO orderbook_market_trades "
                "(session_id, symbol, taker_order_id, maker_order_id, taker_trader_id, maker_trader_id, "
                "taker_side, price, quantity, notional) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
                {
                    sessionId,
                    symbol,
                    std::to_string(trade.takerId),
                    std::to_string(trade.makerId),
                    std::to_string(trade.takerTraderId),
                    std::to_string(trade.makerTraderId),
                    sideToString(trade.takerSide),
                    std::to_string(trade.price),
                    std::to_string(trade.quantity),
                    std::to_string(trade.price * trade.quantity),
                });
        }
    }

    void recordRating(const std::string& lobbyId, const LeaderboardRow& row) {
        execParams(
            "INSERT INTO orderbook_ratings "
            "(lobby_id, trader_id, track, pnl, estimated_value, rating_before, rating_after) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7)",
            {
                lobbyId,
                std::to_string(row.traderId),
                participantTrackToString(row.track),
                jsonDouble(row.pnl),
                jsonDouble(row.estimatedValue),
                std::to_string(row.ratingBefore),
                std::to_string(row.ratingAfter),
            });

        const std::string column = row.track == ParticipantTrack::Bot ? "bot_elo" : "manual_elo";
        execParams(
            "UPDATE orderbook_users SET " + column + " = $1, updated_at = now() WHERE trader_id = $2",
            {std::to_string(row.ratingAfter), std::to_string(row.traderId)});
    }

private:
    std::int64_t appendPersistedEvent(const PersistedEvent& event) const {
        const std::int64_t eventId = execParamsReturningInt64(
            "INSERT INTO orderbook_events "
            "(event_type, session_id, room_id, room_mode, trader_id, track, symbol, side, order_mode, "
            "order_id, price, quantity, accepted, canceled, cooldown_until_epoch, created_at_epoch) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16) "
            "RETURNING id",
            {
                event.eventType,
                event.sessionId,
                event.roomId,
                roomModeToString(event.roomMode),
                std::to_string(event.traderId),
                participantTrackToString(event.track),
                event.symbol,
                sideToString(event.side),
                event.orderMode,
                event.orderId == 0 ? "" : std::to_string(event.orderId),
                event.price == 0 ? "" : std::to_string(event.price),
                event.quantity == 0 ? "" : std::to_string(event.quantity),
                event.accepted ? "true" : "false",
                event.canceled ? "true" : "false",
                event.cooldownUntil ? std::to_string(epochSeconds(*event.cooldownUntil)) : "",
                std::to_string(epochSeconds(event.createdAt)),
            });

        maybeRecordCheckpoint(eventId);
        return eventId;
    }

    void maybeRecordCheckpoint(std::int64_t eventId) const {
        if (eventId <= 0 || checkpointEveryEvents <= 0 || eventId % checkpointEveryEvents != 0) {
            return;
        }

        execParams(
            "INSERT INTO orderbook_checkpoints (event_id, kind, note) VALUES ($1, $2, $3)",
            {
                std::to_string(eventId),
                "event-watermark",
                "Replayable event-log checkpoint. Full state snapshot restore is the next durability layer.",
            });
    }

    void ensureSchema() {
#ifdef ORDERBOOK_WITH_POSTGRES
        exec(
            "CREATE TABLE IF NOT EXISTS orderbook_users ("
            "trader_id BIGINT PRIMARY KEY,"
            "clerk_subject TEXT UNIQUE NOT NULL,"
            "manual_elo INTEGER NOT NULL DEFAULT 1200,"
            "bot_elo INTEGER NOT NULL DEFAULT 1200,"
            "created_at TIMESTAMPTZ NOT NULL DEFAULT now(),"
            "updated_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");"
            "CREATE TABLE IF NOT EXISTS orderbook_session_events ("
            "id BIGSERIAL PRIMARY KEY,"
            "session_id TEXT NOT NULL,"
            "room_id TEXT NOT NULL,"
            "mode TEXT NOT NULL,"
            "trader_id BIGINT NOT NULL,"
            "track TEXT NOT NULL,"
            "event_type TEXT NOT NULL,"
            "cooldown_until_epoch BIGINT,"
            "created_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");"
            "CREATE TABLE IF NOT EXISTS orderbook_orders ("
            "id BIGSERIAL PRIMARY KEY,"
            "session_id TEXT NOT NULL,"
            "trader_id BIGINT NOT NULL,"
            "order_id BIGINT NOT NULL,"
            "symbol TEXT NOT NULL,"
            "side TEXT NOT NULL,"
            "mode TEXT NOT NULL,"
            "price BIGINT NOT NULL,"
            "quantity BIGINT NOT NULL,"
            "accepted BOOLEAN NOT NULL,"
            "filled_quantity BIGINT NOT NULL,"
            "resting_quantity BIGINT NOT NULL,"
            "notional BIGINT NOT NULL,"
            "message TEXT NOT NULL,"
            "created_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");"
            "CREATE INDEX IF NOT EXISTS orderbook_orders_session_trader_idx "
            "ON orderbook_orders(session_id, trader_id);"
            "CREATE TABLE IF NOT EXISTS orderbook_cancels ("
            "id BIGSERIAL PRIMARY KEY,"
            "session_id TEXT NOT NULL,"
            "trader_id BIGINT NOT NULL,"
            "symbol TEXT NOT NULL,"
            "order_id BIGINT NOT NULL,"
            "canceled BOOLEAN NOT NULL,"
            "created_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");"
            "CREATE INDEX IF NOT EXISTS orderbook_cancels_session_trader_idx "
            "ON orderbook_cancels(session_id, trader_id);"
            "CREATE TABLE IF NOT EXISTS orderbook_market_trades ("
            "id BIGSERIAL PRIMARY KEY,"
            "session_id TEXT NOT NULL,"
            "symbol TEXT NOT NULL,"
            "taker_order_id BIGINT NOT NULL,"
            "maker_order_id BIGINT NOT NULL,"
            "taker_trader_id BIGINT NOT NULL,"
            "maker_trader_id BIGINT NOT NULL,"
            "taker_side TEXT NOT NULL,"
            "price BIGINT NOT NULL,"
            "quantity BIGINT NOT NULL,"
            "notional BIGINT NOT NULL,"
            "created_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");"
            "CREATE INDEX IF NOT EXISTS orderbook_trades_session_symbol_idx "
            "ON orderbook_market_trades(session_id, symbol);"
            "CREATE TABLE IF NOT EXISTS orderbook_ratings ("
            "id BIGSERIAL PRIMARY KEY,"
            "lobby_id TEXT NOT NULL,"
            "trader_id BIGINT NOT NULL,"
            "track TEXT NOT NULL,"
            "pnl DOUBLE PRECISION NOT NULL,"
            "estimated_value DOUBLE PRECISION NOT NULL,"
            "rating_before INTEGER NOT NULL,"
            "rating_after INTEGER NOT NULL,"
            "created_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");"
            "CREATE TABLE IF NOT EXISTS orderbook_events ("
            "id BIGSERIAL PRIMARY KEY,"
            "event_type TEXT NOT NULL,"
            "session_id TEXT NOT NULL,"
            "room_id TEXT NOT NULL,"
            "room_mode TEXT NOT NULL,"
            "trader_id BIGINT NOT NULL,"
            "track TEXT,"
            "symbol TEXT,"
            "side TEXT,"
            "order_mode TEXT,"
            "order_id BIGINT,"
            "price BIGINT,"
            "quantity BIGINT,"
            "accepted BOOLEAN NOT NULL DEFAULT false,"
            "canceled BOOLEAN NOT NULL DEFAULT false,"
            "cooldown_until_epoch BIGINT,"
            "created_at_epoch BIGINT NOT NULL,"
            "created_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");"
            "CREATE INDEX IF NOT EXISTS orderbook_events_session_idx "
            "ON orderbook_events(session_id, id);"
            "CREATE TABLE IF NOT EXISTS orderbook_checkpoints ("
            "id BIGSERIAL PRIMARY KEY,"
            "event_id BIGINT NOT NULL,"
            "kind TEXT NOT NULL,"
            "note TEXT NOT NULL,"
            "created_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");");
#endif
    }

    void exec(const std::string& sql) {
#ifdef ORDERBOOK_WITH_POSTGRES
        if (connection == nullptr) {
            return;
        }

        PGresult* result = PQexec(connection, sql.c_str());
        const ExecStatusType status = PQresultStatus(result);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            std::cerr << "PostgreSQL query failed: " << PQerrorMessage(connection) << "\n";
        }

        PQclear(result);
#else
        (void)sql;
#endif
    }

    void execParams(const std::string& sql, const std::vector<std::string>& params) const {
#ifdef ORDERBOOK_WITH_POSTGRES
        if (connection == nullptr) {
            return;
        }

        std::vector<const char*> values;
        values.reserve(params.size());
        for (const std::string& param : params) {
            values.push_back(param.empty() ? nullptr : param.c_str());
        }

        PGresult* result = PQexecParams(
            connection,
            sql.c_str(),
            static_cast<int>(values.size()),
            nullptr,
            values.data(),
            nullptr,
            nullptr,
            0);
        if (PQresultStatus(result) != PGRES_COMMAND_OK) {
            std::cerr << "PostgreSQL write failed: " << PQerrorMessage(connection) << "\n";
        }

        PQclear(result);
#else
        (void)sql;
        (void)params;
#endif
    }

    std::int64_t execParamsReturningInt64(const std::string& sql, const std::vector<std::string>& params) const {
#ifdef ORDERBOOK_WITH_POSTGRES
        if (connection == nullptr) {
            return 0;
        }

        std::vector<const char*> values;
        values.reserve(params.size());
        for (const std::string& param : params) {
            values.push_back(param.empty() ? nullptr : param.c_str());
        }

        PGresult* result = PQexecParams(
            connection,
            sql.c_str(),
            static_cast<int>(values.size()),
            nullptr,
            values.data(),
            nullptr,
            nullptr,
            0);
        if (PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) != 1) {
            std::cerr << "PostgreSQL write failed: " << PQerrorMessage(connection) << "\n";
            PQclear(result);
            return 0;
        }

        const std::int64_t value = std::stoll(PQgetvalue(result, 0, 0));
        PQclear(result);
        return value;
#else
        (void)sql;
        (void)params;
        return 0;
#endif
    }

    int checkpointEveryEvents = 100;

#ifdef ORDERBOOK_WITH_POSTGRES
    PGconn* connection = nullptr;
#endif
};

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

std::optional<ParsedRoomPath> parseRoomPath(const std::string& path) {
    const std::string prefix = "/rooms/";
    if (path.rfind(prefix, 0) != 0 || path.size() <= prefix.size()) {
        return std::nullopt;
    }

    const std::size_t roomStart = prefix.size();
    const std::size_t slash = path.find('/', roomStart);
    if (slash == std::string::npos) {
        return ParsedRoomPath{urlDecode(path.substr(roomStart)), ""};
    }

    return ParsedRoomPath{urlDecode(path.substr(roomStart, slash - roomStart)), path.substr(slash)};
}

std::optional<ParsedLobbyPath> parseLobbyPath(const std::string& path) {
    const std::string prefix = "/lobbies/";
    if (path.rfind(prefix, 0) != 0 || path.size() <= prefix.size()) {
        return std::nullopt;
    }

    const std::size_t lobbyStart = prefix.size();
    const std::size_t slash = path.find('/', lobbyStart);
    if (slash == std::string::npos) {
        return ParsedLobbyPath{urlDecode(path.substr(lobbyStart)), ""};
    }

    return ParsedLobbyPath{urlDecode(path.substr(lobbyStart, slash - lobbyStart)), path.substr(slash)};
}

HttpResponse jsonResponse(int status, const std::string& body) {
    return {status, "application/json", body};
}

bool hasSymbol(const Exchange& exchange, const std::string& symbol) {
    const std::vector<std::string> symbols = exchange.symbols();
    return std::find(symbols.begin(), symbols.end(), symbol) != symbols.end();
}

SimulatorTickResult advanceSinglePlayerSimulator(GameRoom& room, int steps);

std::optional<std::int64_t> marketBuyNotionalEstimate(const Exchange& exchange, const std::string& symbol, Qty quantity) {
    BookSnapshot snapshot = exchange.snapshot(symbol, std::numeric_limits<std::size_t>::max());
    Qty remaining = quantity;
    std::int64_t notional = 0;

    for (const BookLevel& ask : snapshot.asks) {
        if (remaining <= 0) {
            break;
        }

        const Qty tradeQuantity = std::min(remaining, ask.quantity);
        remaining -= tradeQuantity;
        notional += ask.price * tradeQuantity;
    }

    if (remaining > 0) {
        return std::nullopt;
    }

    return notional;
}

void ensureRiskAllowed(
    const GameRoom& room,
    TraderId traderId,
    Side side,
    const ParsedOrder& order,
    bool market,
    std::optional<OrderId> replacedOrderId = std::nullopt) {
    if (!hasSymbol(room.exchange, order.symbol)) {
        throw std::runtime_error("symbol is not allowed in this room");
    }

    if (side == Side::Buy) {
        const std::int64_t requiredCash = market
            ? marketBuyNotionalEstimate(room.exchange, order.symbol, order.quantity).value_or(-1)
            : order.price * order.quantity;
        if (requiredCash < 0) {
            throw std::runtime_error("not enough sell liquidity to estimate this market buy");
        }

        const std::int64_t cash = room.startingCash + room.accounts.cashFlowForTrader(traderId);
        const std::int64_t reservedCash = reservedBuyCashForTrader(room.exchange, traderId, replacedOrderId);
        const std::int64_t availableCash = cash - reservedCash;
        if (requiredCash > availableCash) {
            throw std::runtime_error(
                "not enough cash: available " + std::to_string(availableCash)
                + ", required " + std::to_string(requiredCash));
        }

        return;
    }

    const Qty ownedQuantity = room.accounts.positionQuantityForTrader(traderId, order.symbol);
    const Qty reservedQuantity = reservedOpenSellQuantityForTrader(room.exchange, traderId, order.symbol, replacedOrderId);
    const Qty availableQuantity = ownedQuantity - reservedQuantity;
    if (order.quantity > availableQuantity) {
        throw std::runtime_error(
            "not enough position: available " + std::to_string(availableQuantity)
            + ", required " + std::to_string(order.quantity));
    }
}

PortfolioRecord portfolioWithReservations(const GameRoom& room, TraderId traderId) {
    Qty reservedSellQuantity = 0;
    for (const std::string& symbol : room.exchange.symbols()) {
        reservedSellQuantity += reservedOpenSellQuantityForTrader(room.exchange, traderId, symbol);
    }

    return room.accounts.portfolioForTrader(
        traderId,
        room.startingCash,
        reservedBuyCashForTrader(room.exchange, traderId),
        reservedSellQuantity);
}

HttpResponse orderResultResponse(
    AccountStore& accounts,
    PersistenceStore& persistence,
    const std::string& sessionId,
    const std::string& roomId,
    RoomMode roomMode,
    const std::string& symbol,
    const ParsedOrder& order,
    TraderId traderId,
    Side side,
    const std::string& mode,
    const SubmitResult& result) {
    accounts.recordTrades(symbol, result);
    persistence.recordOrder(sessionId, roomId, roomMode, traderId, side, mode, order, result);
    persistence.recordTrades(sessionId, symbol, result);
    return jsonResponse(200, serializeSubmitResult(result));
}

HttpResponse handleGetMe(
    GameRoom& room,
    const std::string& path,
    const HttpRequest& request) {
    Exchange& exchange = room.exchange;
    AccountStore& accounts = room.accounts;

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
            return jsonResponse(200, serializePortfolio(portfolioWithReservations(room, traderId)));
        }
    } catch (const AuthError& ex) {
        return jsonResponse(401, jsonError(ex.what()));
    } catch (const std::exception& ex) {
        return jsonResponse(400, jsonError(ex.what()));
    }

    return jsonResponse(404, jsonError("unknown GET endpoint"));
}

HttpResponse handlePostOrder(
    GameRoom& room,
    PersistenceStore& persistence,
    const std::string& sessionId,
    const std::string& path,
    const HttpRequest& request) {
    Exchange& exchange = room.exchange;
    AccountStore& accounts = room.accounts;
    OrderIdGenerator& orderIds = room.orderIds;

    try {
        const TraderId traderId = authenticatedTraderId(request);
        const std::string& body = request.body;

        if (path == "/orders/buy") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            ensureRiskAllowed(room, traderId, Side::Buy, order, false);
            order.orderId = orderIds.next();
            return orderResultResponse(
                accounts,
                persistence,
                sessionId,
                room.id,
                room.mode,
                order.symbol,
                order,
                traderId,
                Side::Buy,
                "limit",
                exchange.buy(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/sell") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            ensureRiskAllowed(room, traderId, Side::Sell, order, false);
            order.orderId = orderIds.next();
            return orderResultResponse(
                accounts,
                persistence,
                sessionId,
                room.id,
                room.mode,
                order.symbol,
                order,
                traderId,
                Side::Sell,
                "limit",
                exchange.sell(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/market-buy") {
            ParsedOrder order = parseNewMarketOrder(body);
            ensureRiskAllowed(room, traderId, Side::Buy, order, true);
            order.orderId = orderIds.next();
            return orderResultResponse(
                accounts,
                persistence,
                sessionId,
                room.id,
                room.mode,
                order.symbol,
                order,
                traderId,
                Side::Buy,
                "market",
                exchange.marketBuy(order.symbol, traderId, order.orderId, order.quantity));
        }

        if (path == "/orders/market-sell") {
            ParsedOrder order = parseNewMarketOrder(body);
            ensureRiskAllowed(room, traderId, Side::Sell, order, true);
            order.orderId = orderIds.next();
            return orderResultResponse(
                accounts,
                persistence,
                sessionId,
                room.id,
                room.mode,
                order.symbol,
                order,
                traderId,
                Side::Sell,
                "market",
                exchange.marketSell(order.symbol, traderId, order.orderId, order.quantity));
        }

        if (path == "/orders/ioc-buy") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            ensureRiskAllowed(room, traderId, Side::Buy, order, false);
            order.orderId = orderIds.next();
            return orderResultResponse(
                accounts,
                persistence,
                sessionId,
                room.id,
                room.mode,
                order.symbol,
                order,
                traderId,
                Side::Buy,
                "ioc",
                exchange.iocBuy(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/ioc-sell") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            ensureRiskAllowed(room, traderId, Side::Sell, order, false);
            order.orderId = orderIds.next();
            return orderResultResponse(
                accounts,
                persistence,
                sessionId,
                room.id,
                room.mode,
                order.symbol,
                order,
                traderId,
                Side::Sell,
                "ioc",
                exchange.iocSell(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/fok-buy") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            ensureRiskAllowed(room, traderId, Side::Buy, order, false);
            order.orderId = orderIds.next();
            return orderResultResponse(
                accounts,
                persistence,
                sessionId,
                room.id,
                room.mode,
                order.symbol,
                order,
                traderId,
                Side::Buy,
                "fok",
                exchange.fokBuy(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/fok-sell") {
            ParsedOrder order = parseNewOrderWithPrice(body);
            ensureRiskAllowed(room, traderId, Side::Sell, order, false);
            order.orderId = orderIds.next();
            return orderResultResponse(
                accounts,
                persistence,
                sessionId,
                room.id,
                room.mode,
                order.symbol,
                order,
                traderId,
                Side::Sell,
                "fok",
                exchange.fokSell(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/replace-buy") {
            const ParsedOrder order = parseOrderWithPrice(body);
            ensureRiskAllowed(room, traderId, Side::Buy, order, false, order.orderId);
            return orderResultResponse(
                accounts,
                persistence,
                sessionId,
                room.id,
                room.mode,
                order.symbol,
                order,
                traderId,
                Side::Buy,
                "replace",
                exchange.replaceBuy(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/replace-sell") {
            const ParsedOrder order = parseOrderWithPrice(body);
            ensureRiskAllowed(room, traderId, Side::Sell, order, false, order.orderId);
            return orderResultResponse(
                accounts,
                persistence,
                sessionId,
                room.id,
                room.mode,
                order.symbol,
                order,
                traderId,
                Side::Sell,
                "replace",
                exchange.replaceSell(order.symbol, traderId, order.orderId, order.price, order.quantity));
        }

        if (path == "/orders/cancel") {
            const auto symbol = jsonStringField(body, "symbol");
            const auto orderId = jsonIntField(body, "orderId");

            if (!symbol || !orderId) {
                throw std::runtime_error("expected symbol and orderId");
            }

            const bool canceled = exchange.cancelForTrader(*symbol, traderId, *orderId);
            persistence.recordCancel(sessionId, room.id, room.mode, traderId, *symbol, *orderId, canceled);
            return jsonResponse(200, std::string("{\"canceled\":") + (canceled ? "true" : "false") + "}");
        }
    } catch (const AuthError& ex) {
        return jsonResponse(401, jsonError(ex.what()));
    } catch (const std::exception& ex) {
        return jsonResponse(400, jsonError(ex.what()));
    }

    return jsonResponse(404, jsonError("unknown POST endpoint"));
}

HttpResponse routeRoom(
    GameRoom& room,
    PersistenceStore& persistence,
    const std::string& sessionId,
    const HttpRequest& request) {
    const auto [path, query] = splitQuery(request.path);
    Exchange& exchange = room.exchange;
    AccountStore& accounts = room.accounts;

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
            return handleGetMe(room, path, request);
        }

        return jsonResponse(404, jsonError("unknown GET endpoint"));
    }

    if (request.method == "POST") {
        return handlePostOrder(room, persistence, sessionId, path, request);
    }

    return jsonResponse(405, jsonError("only GET, POST, and OPTIONS are supported"));
}

void cancelTraderOrders(Exchange& exchange, TraderId traderId) {
    for (const std::string& symbol : exchange.symbols()) {
        const std::vector<OpenOrder> orders = exchange.openOrders(symbol, traderId);
        for (const OpenOrder& order : orders) {
            exchange.cancelForTrader(symbol, traderId, order.orderId);
        }
    }
}

HttpResponse routeLobby(
    RoomStore& rooms,
    CompetitiveLobby& lobby,
    PersistenceStore& persistence,
    const HttpRequest& request) {
    const auto [path, query] = splitQuery(request.path);

    if (request.method == "OPTIONS") {
        return jsonResponse(200, "{}");
    }

    try {
        if (request.method == "GET" && path == "/membership") {
            const AuthenticatedUser user = authenticatedUser(request);
            persistence.upsertUser(user);
            const std::optional<LobbyMembershipRecord> membership = lobby.membershipFor(user.traderId);
            return jsonResponse(
                200,
                std::string("{\"joined\":")
                    + (membership && rooms.hasActiveSession(user.traderId, lobby.id) ? "true" : "false")
                    + ",\"traderId\":" + std::to_string(user.traderId)
                    + ",\"track\":\"" + participantTrackToString(membership ? membership->track : ParticipantTrack::Manual) + "\""
                    + ",\"cooldownRemainingSeconds\":" + std::to_string(rooms.cooldownRemainingSeconds(user.traderId))
                    + ",\"activeSession\":" + serializeActiveSession(rooms.activeSessionFor(user.traderId))
                    + ",\"lobby\":" + serializeLobby(lobby)
                    + "}");
        }

        if (request.method == "POST" && path == "/join") {
            const AuthenticatedUser user = authenticatedUser(request);
            persistence.upsertUser(user);
            const ParticipantTrack track = parseParticipantTrack(jsonStringField(request.body, "track").value_or("manual"));
            const LobbyJoinOutcome outcome = rooms.joinCompetitiveLobby(lobby.id, user.traderId, track);
            if (outcome.result == LobbyJoinResult::Joined) {
                persistence.recordSessionEvent(lobby.id, lobby.roomId, RoomMode::Competitive, user.traderId, track, "join");
            }

            const int status = (outcome.result == LobbyJoinResult::Joined || outcome.result == LobbyJoinResult::AlreadyJoined)
                ? 200
                : 409;
            return jsonResponse(
                status,
                serializeJoinOutcome(outcome, user.traderId, rooms.activeSessionFor(user.traderId), "\"lobby\":" + serializeLobby(lobby)));
        }

        if (request.method == "POST" && path == "/leave") {
            const AuthenticatedUser user = authenticatedUser(request);
            persistence.upsertUser(user);
            const std::optional<LobbyMembershipRecord> membership = lobby.membershipFor(user.traderId);
            const ParticipantTrack track = membership ? membership->track : ParticipantTrack::Manual;
            const bool left = rooms.leaveCompetitiveLobby(lobby.id, user.traderId);
            if (left) {
                persistence.recordSessionEvent(
                    lobby.id,
                    lobby.roomId,
                    RoomMode::Competitive,
                    user.traderId,
                    track,
                    "leave",
                    Clock::now() + std::chrono::seconds(rooms.config().rejoinCooldownSeconds));
            }

            return jsonResponse(
                200,
                std::string("{\"left\":") + (left ? "true" : "false")
                    + ",\"cooldownRemainingSeconds\":" + std::to_string(rooms.cooldownRemainingSeconds(user.traderId))
                    + ",\"activeSession\":" + serializeActiveSession(rooms.activeSessionFor(user.traderId))
                    + ",\"lobby\":" + serializeLobby(lobby) + "}");
        }

        if (request.method == "GET" && path == "/leaderboard") {
            const TraderId traderId = authenticatedTraderId(request);
            if (!lobby.wasAdmitted(traderId)) {
                return jsonResponse(403, jsonError("join this lobby before viewing its leaderboard"));
            }

            lobby.finalizeIfNeeded(rooms.ratings(ParticipantTrack::Manual), rooms.ratings(ParticipantTrack::Bot));
            if (lobby.hasFinalRatings() && rooms.storeLeaderboardRatings(lobby)) {
                for (const LeaderboardRow& row : lobby.leaderboardRows()) {
                    persistence.recordRating(lobby.id, row);
                }
            }

            return jsonResponse(200, serializeLeaderboard(lobby.leaderboardRows()));
        }

        const bool requiresMembership = request.method == "POST" || request.method == "GET";
        if (requiresMembership) {
            const TraderId traderId = authenticatedTraderId(request);
            if (!lobby.contains(traderId) || !rooms.hasActiveSession(traderId, lobby.id)) {
                return jsonResponse(403, jsonError("join this lobby before viewing or trading"));
            }

            const LobbyPhase phase = lobby.currentPhase();
            if (request.method == "POST" && phase != LobbyPhase::Running) {
                return jsonResponse(
                    409,
                    jsonError(phase == LobbyPhase::Finished ? "game is finished" : "game has not started yet"));
            }
        }
    } catch (const AuthError& ex) {
        return jsonResponse(401, jsonError(ex.what()));
    } catch (const std::exception& ex) {
        return jsonResponse(400, jsonError(ex.what()));
    }

    HttpRequest scopedRequest = request;
    scopedRequest.path = path + (query.empty() ? "" : "?" + query);
    return routeRoom(*lobby.game, persistence, lobby.id, scopedRequest);
}

HttpResponse route(RoomStore& rooms, PersistenceStore& persistence, const HttpRequest& request) {
    const auto [path, query] = splitQuery(request.path);

    if (request.method == "OPTIONS") {
        return jsonResponse(200, "{}");
    }

    if (request.method == "GET") {
        if (path == "/health") {
            return jsonResponse(200, "{\"ok\":true}");
        }

        if (path == "/rooms") {
            return jsonResponse(200, serializeRooms(rooms.rooms(), false));
        }

        if (path == "/lobbies") {
            return jsonResponse(200, serializeLobbies(rooms.lobbies()));
        }

        if (path == "/me/session") {
            try {
                const AuthenticatedUser user = authenticatedUser(request);
                persistence.upsertUser(user);
                return jsonResponse(
                    200,
                    std::string("{\"traderId\":") + std::to_string(user.traderId)
                        + ",\"cooldownRemainingSeconds\":"
                        + std::to_string(rooms.cooldownRemainingSeconds(user.traderId))
                        + ",\"activeSession\":" + serializeActiveSession(rooms.activeSessionFor(user.traderId))
                        + "}");
            } catch (const AuthError& ex) {
                return jsonResponse(401, jsonError(ex.what()));
            }
        }
    }

    const std::optional<ParsedRoomPath> roomPath = parseRoomPath(path);
    if (roomPath) {
        GameRoom* room = rooms.find(roomPath->roomId);
        if (room == nullptr) {
            return jsonResponse(404, jsonError("unknown room"));
        }

        if (roomPath->nestedPath.empty()) {
            if (request.method == "GET") {
                return jsonResponse(200, serializeRoom(*room, false));
            }

            return jsonResponse(405, jsonError("room detail only supports GET"));
        }

        if (roomPath->nestedPath == "/lobbies") {
            if (request.method == "GET") {
                return jsonResponse(200, serializeLobbies(rooms.lobbiesForRoom(room->id)));
            }

            return jsonResponse(405, jsonError("room lobbies only support GET"));
        }

        if (room->mode == RoomMode::Competitive) {
            return jsonResponse(409, jsonError("select a competitive lobby"));
        }

        try {
            if (request.method == "GET" && roomPath->nestedPath == "/membership") {
                const AuthenticatedUser user = authenticatedUser(request);
                persistence.upsertUser(user);
                const std::string sessionId = room->id + "-" + std::to_string(user.traderId);
                const bool joined = rooms.hasActiveSession(user.traderId, sessionId);
                return jsonResponse(
                    200,
                    std::string("{\"joined\":") + (joined ? "true" : "false")
                        + ",\"traderId\":" + std::to_string(user.traderId)
                        + ",\"cooldownRemainingSeconds\":"
                        + std::to_string(rooms.cooldownRemainingSeconds(user.traderId))
                        + ",\"activeSession\":" + serializeActiveSession(rooms.activeSessionFor(user.traderId))
                        + ",\"room\":" + serializeRoom(*room, joined)
                        + "}");
            }

            if (request.method == "POST" && roomPath->nestedPath == "/join") {
                const AuthenticatedUser user = authenticatedUser(request);
                persistence.upsertUser(user);
                const LobbyJoinOutcome outcome = rooms.joinSingleRoom(room->id, user.traderId);
                if (outcome.result == LobbyJoinResult::Joined) {
                    const std::string sessionId = room->id + "-" + std::to_string(user.traderId);
                    persistence.recordSessionEvent(sessionId, room->id, RoomMode::Single, user.traderId, ParticipantTrack::Manual, "join");
                }

                const int status = (outcome.result == LobbyJoinResult::Joined || outcome.result == LobbyJoinResult::AlreadyJoined)
                    ? 200
                    : 409;
                return jsonResponse(
                    status,
                    serializeJoinOutcome(
                        outcome,
                        user.traderId,
                        rooms.activeSessionFor(user.traderId),
                        "\"room\":" + serializeRoom(*room, status == 200)));
            }

            if (request.method == "POST" && roomPath->nestedPath == "/leave") {
                const AuthenticatedUser user = authenticatedUser(request);
                persistence.upsertUser(user);
                const bool left = rooms.leaveSingleRoom(room->id, user.traderId);
                if (left) {
                    const std::string sessionId = room->id + "-" + std::to_string(user.traderId);
                    persistence.recordSessionEvent(
                        sessionId,
                        room->id,
                        RoomMode::Single,
                        user.traderId,
                        ParticipantTrack::Manual,
                        "leave",
                        Clock::now() + std::chrono::seconds(rooms.config().rejoinCooldownSeconds));
                }

                return jsonResponse(
                    200,
                    std::string("{\"left\":") + (left ? "true" : "false")
                        + ",\"cooldownRemainingSeconds\":"
                        + std::to_string(rooms.cooldownRemainingSeconds(user.traderId))
                        + ",\"activeSession\":" + serializeActiveSession(rooms.activeSessionFor(user.traderId))
                        + ",\"room\":" + serializeRoom(*room, false)
                        + "}");
            }

            const AuthenticatedUser user = authenticatedUser(request);
            const std::string sessionId = room->id + "-" + std::to_string(user.traderId);
            if (!rooms.hasActiveSession(user.traderId, sessionId)) {
                return jsonResponse(403, jsonError("enter this room before viewing or trading"));
            }

            SinglePlayerSession* session = rooms.findSingleSession(room->id, user.traderId);
            if (session == nullptr || !session->active) {
                return jsonResponse(403, jsonError("enter this room before viewing or trading"));
            }

            if (request.method == "POST" && roomPath->nestedPath == "/simulator/tick") {
                if (room->mode != RoomMode::Single) {
                    return jsonResponse(400, jsonError("simulator ticks are only available in single-player rooms"));
                }

                const int steps = static_cast<int>(std::max<std::int64_t>(
                    1,
                    std::min<std::int64_t>(jsonIntField(request.body, "steps").value_or(1), 25)));
                const SimulatorTickResult result = advanceSinglePlayerSimulator(*session->game, steps);
                persistence.recordSimulatorTick(session->id, room->id, user.traderId, result.steps);
                return jsonResponse(200, serializeSimulatorTickResult(result));
            }

            HttpRequest scopedRequest = request;
            scopedRequest.path = roomPath->nestedPath + (query.empty() ? "" : "?" + query);
            return routeRoom(*session->game, persistence, session->id, scopedRequest);
        } catch (const AuthError& ex) {
            return jsonResponse(401, jsonError(ex.what()));
        } catch (const std::exception& ex) {
            return jsonResponse(400, jsonError(ex.what()));
        }
    }

    const std::optional<ParsedLobbyPath> lobbyPath = parseLobbyPath(path);
    if (lobbyPath) {
        CompetitiveLobby* lobby = rooms.findLobby(lobbyPath->lobbyId);
        if (lobby == nullptr) {
            return jsonResponse(404, jsonError("unknown lobby"));
        }

        if (lobbyPath->nestedPath.empty()) {
            if (request.method == "GET") {
                return jsonResponse(200, serializeLobby(*lobby));
            }

            return jsonResponse(405, jsonError("lobby detail only supports GET"));
        }

        HttpRequest scopedRequest = request;
        scopedRequest.path = lobbyPath->nestedPath + (query.empty() ? "" : "?" + query);
        return routeLobby(rooms, *lobby, persistence, scopedRequest);
    }

    (void)rooms;
    return jsonResponse(409, jsonError("select and enter a room or lobby first"));
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

constexpr TraderId HouseTraderId = 9'000'000'001LL;

double deterministicNoise(const std::string& symbol, std::uint64_t tick, int salt) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : symbol) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }

    hash ^= (tick + 0x9e3779b97f4a7c15ULL + static_cast<std::uint64_t>(salt) * 0xbf58476d1ce4e5b9ULL);
    hash *= 1099511628211ULL;
    const double unit = static_cast<double>(hash % 20001ULL) / 10000.0;
    return unit - 1.0;
}

void seedDefaultBooks(Exchange& exchange) {
    exchange.ensureBook("BTC-USD");
    exchange.ensureBook("ETH-USD");
}

void seedHouseLiquidity(GameRoom& room, const AssetConfig& asset, int levels, Qty baseQuantity) {
    room.exchange.ensureBook(asset.symbol);

    if (!room.houseLiquidity || asset.referencePrice <= 2) {
        return;
    }

    const Price baseSpread = std::max<Price>(1, asset.referencePrice / 100);
    for (int level = 0; level < levels; ++level) {
        const Price offset = baseSpread + level;
        const Qty quantity = baseQuantity + static_cast<Qty>(level * baseQuantity / 2);
        const Price bid = std::max<Price>(1, asset.referencePrice - offset);
        const Price ask = asset.referencePrice + offset;

        room.exchange.buy(asset.symbol, HouseTraderId, room.orderIds.next(), bid, quantity);
        room.exchange.sell(asset.symbol, HouseTraderId, room.orderIds.next(), ask, quantity);
    }
}

void initializeSimulator(GameRoom& room) {
    for (const AssetConfig& asset : room.assets) {
        room.simulator[asset.symbol] = {0, static_cast<double>(asset.referencePrice)};
    }
}

void cancelHouseOrdersForSymbol(GameRoom& room, const std::string& symbol) {
    const std::vector<OpenOrder> orders = room.exchange.openOrders(symbol, HouseTraderId);
    for (const OpenOrder& order : orders) {
        room.exchange.cancelForTrader(symbol, HouseTraderId, order.orderId);
    }
}

double nextFairValue(const AssetConfig& asset, SimulatorSymbolState& state) {
    ++state.tick;

    const double reference = static_cast<double>(asset.referencePrice);
    const double noise = deterministicNoise(asset.symbol, state.tick, 1);
    double drift = 0.0;

    if (asset.behavior == "trend-cycle") {
        drift = 0.10 + std::sin(static_cast<double>(state.tick) / 5.0) * 0.35 + noise * 0.18;
    } else if (asset.behavior == "mean-reverting") {
        drift = (reference - state.fairValue) * 0.14
            + std::sin(static_cast<double>(state.tick) / 4.0) * 0.22
            + noise * 0.16;
    } else if (asset.behavior == "factor-noisy") {
        drift = std::sin(static_cast<double>(state.tick) / 7.0) * 0.28 + noise * 0.50;
    } else if (asset.behavior == "noise-trap") {
        drift = noise * 0.90;
    } else {
        drift = noise * 0.20;
    }

    state.fairValue = std::clamp(state.fairValue + drift, std::max(2.0, reference * 0.25), reference * 4.0);
    return state.fairValue;
}

void quoteAroundFairValue(GameRoom& room, const AssetConfig& asset, double fairValue, SimulatorTickResult& tickResult) {
    cancelHouseOrdersForSymbol(room, asset.symbol);

    const Price fair = std::max<Price>(1, static_cast<Price>(std::llround(fairValue)));
    const Price baseSpread = std::max<Price>(
        1,
        asset.behavior == "noise-trap" ? std::max<Price>(2, fair / 35) : std::max<Price>(1, fair / 100));
    const int levels = asset.behavior == "noise-trap" ? 3 : 5;
    const Qty baseQuantity = asset.behavior == "noise-trap" ? 22 : 55;

    for (int level = 0; level < levels; ++level) {
        const Price offset = baseSpread + level;
        const Qty quantity = baseQuantity + static_cast<Qty>(level * baseQuantity / 2);
        const Price bid = std::max<Price>(1, fair - offset);
        const Price ask = fair + offset;

        SubmitResult buy = room.exchange.buy(asset.symbol, HouseTraderId, room.orderIds.next(), bid, quantity);
        room.accounts.recordTrades(asset.symbol, buy);
        tickResult.trades += buy.trades.size();

        SubmitResult sell = room.exchange.sell(asset.symbol, HouseTraderId, room.orderIds.next(), ask, quantity);
        room.accounts.recordTrades(asset.symbol, sell);
        tickResult.trades += sell.trades.size();
    }
}

SimulatorTickResult advanceSinglePlayerSimulator(GameRoom& room, int steps) {
    SimulatorTickResult result;
    if (room.mode != RoomMode::Single || !room.houseLiquidity) {
        return result;
    }

    result.steps = std::max(1, std::min(steps, 25));
    for (int step = 0; step < result.steps; ++step) {
        for (const AssetConfig& asset : room.assets) {
            SimulatorSymbolState& state = room.simulator[asset.symbol];
            if (state.fairValue <= 0.0) {
                state.fairValue = static_cast<double>(asset.referencePrice);
            }

            quoteAroundFairValue(room, asset, nextFairValue(asset, state), result);
        }
    }

    result.symbols.reserve(room.assets.size());
    for (const AssetConfig& asset : room.assets) {
        result.symbols.push_back(asset.symbol);
    }

    return result;
}

void seedRoom(GameRoom& room) {
    initializeSimulator(room);
    for (const AssetConfig& asset : room.assets) {
        seedHouseLiquidity(room, asset, room.mode == RoomMode::Single ? 5 : 3, room.mode == RoomMode::Single ? 60 : 25);
    }
}

std::unique_ptr<GameRoom> makeSandboxRoom(const GameConfig& config) {
    auto room = std::make_unique<GameRoom>();
    room->id = "sandbox";
    room->name = "Sandbox";
    room->mode = RoomMode::Single;
    room->difficulty = "dev";
    room->startingCash = config.startingCash;
    room->maxParticipants = 1;
    room->startDelaySeconds = 0;
    room->gameDurationSeconds = config.gameDurationSeconds;
    room->rejoinCooldownSeconds = config.rejoinCooldownSeconds;
    room->houseLiquidity = false;
    room->assets = {
        {"BTC-USD", "Sandbox BTC", "manual-testing", "sandbox", 100, "none"},
        {"ETH-USD", "Sandbox ETH", "manual-testing", "sandbox", 50, "none"},
    };
    seedDefaultBooks(room->exchange);
    return room;
}

std::unique_ptr<GameRoom> makeSinglePlayerRoom(const GameConfig& config) {
    auto room = std::make_unique<GameRoom>();
    room->id = "solo-alpha";
    room->name = "Solo Alpha Lab";
    room->mode = RoomMode::Single;
    room->difficulty = "medium";
    room->startingCash = config.startingCash;
    room->maxParticipants = 1;
    room->startDelaySeconds = 0;
    room->gameDurationSeconds = config.gameDurationSeconds;
    room->rejoinCooldownSeconds = config.rejoinCooldownSeconds;
    room->houseLiquidity = true;
    room->assets = {
        {"NOVA", "Nova Systems", "trend-cycle", "synthetic", 100, "learnable"},
        {"ORBIT", "Orbit Retail", "factor-noisy", "masked-real-series", 64, "moderate"},
        {"LYRA", "Lyra Materials", "mean-reverting", "synthetic", 42, "learnable"},
        {"MIST", "Mist Biotech", "noise-trap", "synthetic", 28, "low"},
    };
    seedRoom(*room);
    return room;
}

std::unique_ptr<GameRoom> makeCompetitiveRoom(const GameConfig& config) {
    auto room = std::make_unique<GameRoom>();
    room->id = "comp-aurora";
    room->name = "Aurora Competitive";
    room->mode = RoomMode::Competitive;
    room->difficulty = "hard";
    room->startingCash = config.startingCash;
    room->maxParticipants = 20;
    room->startDelaySeconds = config.startDelaySeconds;
    room->gameDurationSeconds = config.gameDurationSeconds;
    room->rejoinCooldownSeconds = config.rejoinCooldownSeconds;
    room->houseLiquidity = true;
    room->assets = {
        {"AXON", "Axon Energy", "event-driven", "masked-real-series", 88, "moderate"},
        {"CROWN", "Crown Foods", "seasonal-demand", "synthetic", 73, "learnable"},
        {"QUILL", "Quill Cloud", "regime-shift", "masked-real-series", 120, "hard"},
        {"STATIC", "Static Holdings", "noise-trap", "synthetic", 55, "low"},
    };
    seedRoom(*room);
    return room;
}

std::unique_ptr<CompetitiveLobby> makeCompetitiveLobby(
    const GameRoom& room,
    const std::string& lobbyId,
    const std::string& lobbyName,
    int capacity) {
    auto game = std::make_unique<GameRoom>();
    game->id = lobbyId;
    game->name = lobbyName;
    game->mode = room.mode;
    game->difficulty = room.difficulty;
    game->startingCash = room.startingCash;
    game->maxParticipants = capacity;
    game->startDelaySeconds = room.startDelaySeconds;
    game->gameDurationSeconds = room.gameDurationSeconds;
    game->rejoinCooldownSeconds = room.rejoinCooldownSeconds;
    game->houseLiquidity = room.houseLiquidity;
    game->assets = room.assets;
    seedRoom(*game);

    auto lobby = std::make_unique<CompetitiveLobby>();
    lobby->id = lobbyId;
    lobby->name = lobbyName;
    lobby->roomId = room.id;
    lobby->capacity = capacity;
    lobby->startDelaySeconds = room.startDelaySeconds;
    lobby->gameDurationSeconds = room.gameDurationSeconds;
    lobby->game = std::move(game);
    return lobby;
}

RoomStore::RoomStore(GameConfig config)
    : gameConfig(config) {
    addRoom(makeSandboxRoom(gameConfig));
    addRoom(makeSinglePlayerRoom(gameConfig));

    std::unique_ptr<GameRoom> competitiveRoom = makeCompetitiveRoom(gameConfig);
    addLobby(makeCompetitiveLobby(*competitiveRoom, "aurora-open-10", "Aurora Open 10", 10));
    addLobby(makeCompetitiveLobby(*competitiveRoom, "aurora-open-15", "Aurora Open 15", 15));
    addLobby(makeCompetitiveLobby(*competitiveRoom, "aurora-open-20", "Aurora Open 20", 20));
    addRoom(std::move(competitiveRoom));
}

void RoomStore::addRoom(std::unique_ptr<GameRoom> room) {
    const std::string id = room->id;
    roomById[id] = std::move(room);
}

void RoomStore::addLobby(std::unique_ptr<CompetitiveLobby> lobby) {
    const std::string id = lobby->id;
    lobbyById[id] = std::move(lobby);
}

GameRoom* RoomStore::find(const std::string& roomId) {
    const auto it = roomById.find(roomId);
    if (it == roomById.end()) {
        return nullptr;
    }

    return it->second.get();
}

CompetitiveLobby* RoomStore::findLobby(const std::string& lobbyId) {
    const auto it = lobbyById.find(lobbyId);
    if (it == lobbyById.end()) {
        return nullptr;
    }

    return it->second.get();
}

std::vector<const GameRoom*> RoomStore::rooms() const {
    std::vector<const GameRoom*> result;
    result.reserve(roomById.size());

    for (const auto& [id, room] : roomById) {
        (void)id;
        result.push_back(room.get());
    }

    return result;
}

std::vector<const CompetitiveLobby*> RoomStore::lobbies() const {
    std::vector<const CompetitiveLobby*> result;
    result.reserve(lobbyById.size());

    for (const auto& [id, lobby] : lobbyById) {
        (void)id;
        result.push_back(lobby.get());
    }

    return result;
}

std::vector<const CompetitiveLobby*> RoomStore::lobbiesForRoom(const std::string& roomId) const {
    std::vector<const CompetitiveLobby*> result;

    for (const auto& [id, lobby] : lobbyById) {
        (void)id;
        if (lobby->roomId == roomId) {
            result.push_back(lobby.get());
        }
    }

    return result;
}

SinglePlayerSession* RoomStore::findSingleSession(const std::string& roomId, TraderId traderId) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    const auto roomSessions = singleSessionsByRoom.find(roomId);
    if (roomSessions == singleSessionsByRoom.end()) {
        return nullptr;
    }

    const auto found = roomSessions->second.find(traderId);
    if (found == roomSessions->second.end()) {
        return nullptr;
    }

    return found->second.get();
}

LobbyJoinOutcome RoomStore::joinSingleRoom(const std::string& roomId, TraderId traderId) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    const GameRoom* room = find(roomId);
    if (room == nullptr || room->mode != RoomMode::Single) {
        return makeJoinOutcome(LobbyJoinResult::GameClosed);
    }

    const std::string sessionId = roomId + "-" + std::to_string(traderId);
    LobbyJoinOutcome check = canJoinSessionLocked(traderId, sessionId);
    if (check.result == LobbyJoinResult::AlreadyJoined) {
        return check;
    }

    if (check.result != LobbyJoinResult::Joined) {
        return check;
    }

    auto& sessions = singleSessionsByRoom[roomId];
    auto found = sessions.find(traderId);
    if (found == sessions.end()) {
        auto session = std::make_unique<SinglePlayerSession>();
        session->id = sessionId;
        session->roomId = roomId;
        session->traderId = traderId;
        session->game = cloneSingleRoom(*room);
        session->joinedAt = Clock::now();
        found = sessions.emplace(traderId, std::move(session)).first;
    }

    found->second->active = true;
    found->second->joinedAt = Clock::now();
    markJoinedLocked(traderId, {sessionId, roomId, false});
    return makeJoinOutcome(LobbyJoinResult::Joined);
}

bool RoomStore::leaveSingleRoom(const std::string& roomId, TraderId traderId) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    auto roomSessions = singleSessionsByRoom.find(roomId);
    if (roomSessions == singleSessionsByRoom.end()) {
        return false;
    }

    auto found = roomSessions->second.find(traderId);
    if (found == roomSessions->second.end() || !found->second->active) {
        return false;
    }

    cancelTraderOrders(found->second->game->exchange, traderId);
    found->second->active = false;
    markLeftLocked(traderId);
    return true;
}

LobbyJoinOutcome RoomStore::joinCompetitiveLobby(
    const std::string& lobbyId,
    TraderId traderId,
    ParticipantTrack track) {
    CompetitiveLobby* lobby = findLobby(lobbyId);
    if (lobby == nullptr) {
        return makeJoinOutcome(LobbyJoinResult::GameClosed);
    }

    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        LobbyJoinOutcome check = canJoinSessionLocked(traderId, lobbyId);
        if (check.result == LobbyJoinResult::AlreadyJoined && lobby->contains(traderId)) {
            return check;
        }

        if (check.result != LobbyJoinResult::Joined) {
            return check;
        }
    }

    const LobbyJoinResult joinResult = lobby->join(traderId, track);
    if (joinResult != LobbyJoinResult::Joined && joinResult != LobbyJoinResult::AlreadyJoined) {
        return makeJoinOutcome(joinResult);
    }

    std::lock_guard<std::mutex> lock(sessionMutex);
    markJoinedLocked(traderId, {lobbyId, lobby->roomId, true});
    return makeJoinOutcome(joinResult);
}

bool RoomStore::leaveCompetitiveLobby(const std::string& lobbyId, TraderId traderId) {
    CompetitiveLobby* lobby = findLobby(lobbyId);
    if (lobby == nullptr) {
        return false;
    }

    const bool left = lobby->leave(traderId);
    if (!left) {
        return false;
    }

    cancelTraderOrders(lobby->game->exchange, traderId);

    std::lock_guard<std::mutex> lock(sessionMutex);
    const auto active = activeByTrader.find(traderId);
    if (active != activeByTrader.end() && active->second.id == lobbyId) {
        markLeftLocked(traderId);
    }

    return true;
}

SubmitResult submitPersistedOrder(GameRoom& room, const PersistedEvent& event) {
    room.orderIds.setNextAtLeast(event.orderId + 1);

    if (event.orderMode == "market") {
        return event.side == Side::Buy
            ? room.exchange.marketBuy(event.symbol, event.traderId, event.orderId, event.quantity)
            : room.exchange.marketSell(event.symbol, event.traderId, event.orderId, event.quantity);
    }

    if (event.orderMode == "ioc") {
        return event.side == Side::Buy
            ? room.exchange.iocBuy(event.symbol, event.traderId, event.orderId, event.price, event.quantity)
            : room.exchange.iocSell(event.symbol, event.traderId, event.orderId, event.price, event.quantity);
    }

    if (event.orderMode == "fok") {
        return event.side == Side::Buy
            ? room.exchange.fokBuy(event.symbol, event.traderId, event.orderId, event.price, event.quantity)
            : room.exchange.fokSell(event.symbol, event.traderId, event.orderId, event.price, event.quantity);
    }

    if (event.orderMode == "replace") {
        return event.side == Side::Buy
            ? room.exchange.replaceBuy(event.symbol, event.traderId, event.orderId, event.price, event.quantity)
            : room.exchange.replaceSell(event.symbol, event.traderId, event.orderId, event.price, event.quantity);
    }

    return event.side == Side::Buy
        ? room.exchange.buy(event.symbol, event.traderId, event.orderId, event.price, event.quantity)
        : room.exchange.sell(event.symbol, event.traderId, event.orderId, event.price, event.quantity);
}

RestoreStats restoreFromEvents(RoomStore& rooms, const std::vector<PersistedEvent>& events) {
    RestoreStats stats;

    for (const PersistedEvent& event : events) {
        ++stats.events;

        if (event.eventType == "session_join") {
            if (rooms.restoreSessionJoin(event)) {
                ++stats.sessions;
            } else {
                ++stats.skipped;
            }
            continue;
        }

        if (event.eventType == "session_leave") {
            if (rooms.restoreSessionLeave(event)) {
                ++stats.sessions;
            } else {
                ++stats.skipped;
            }
            continue;
        }

        GameRoom* game = rooms.gameForPersistedSession(event);
        if (game == nullptr) {
            ++stats.skipped;
            continue;
        }

        if (event.eventType == "order_submit") {
            const SubmitResult result = submitPersistedOrder(*game, event);
            if (result.accepted != event.accepted) {
                std::cerr << "Replay warning: order " << event.orderId
                          << " accepted=" << result.accepted
                          << " but persisted accepted=" << event.accepted << "\n";
            }

            game->accounts.recordTrades(event.symbol, result);
            ++stats.orders;
            continue;
        }

        if (event.eventType == "order_cancel") {
            game->orderIds.setNextAtLeast(event.orderId + 1);
            if (event.canceled) {
                const bool replayCanceled = game->exchange.cancelForTrader(event.symbol, event.traderId, event.orderId);
                if (!replayCanceled) {
                    std::cerr << "Replay warning: cancel for order " << event.orderId
                              << " did not find a resting order\n";
                }
            }

            ++stats.cancels;
            continue;
        }

        if (event.eventType == "simulator_tick") {
            const int steps = static_cast<int>(std::max<std::int64_t>(1, std::min<std::int64_t>(event.quantity, 25)));
            advanceSinglePlayerSimulator(*game, steps);
            ++stats.simulatorTicks;
            continue;
        }

        ++stats.skipped;
    }

    return stats;
}

bool RoomStore::hasActiveSession(TraderId traderId, const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(sessionMutex);
    const auto active = activeByTrader.find(traderId);
    return active != activeByTrader.end() && active->second.id == sessionId;
}

std::optional<ActiveSession> RoomStore::activeSessionFor(TraderId traderId) const {
    std::lock_guard<std::mutex> lock(sessionMutex);
    const auto active = activeByTrader.find(traderId);
    if (active == activeByTrader.end()) {
        return std::nullopt;
    }

    return active->second;
}

std::int64_t RoomStore::cooldownRemainingSeconds(TraderId traderId) const {
    std::lock_guard<std::mutex> lock(sessionMutex);
    const auto cooldown = cooldownUntilByTrader.find(traderId);
    if (cooldown == cooldownUntilByTrader.end()) {
        return 0;
    }

    return secondsRemaining(cooldown->second);
}

const GameConfig& RoomStore::config() const {
    return gameConfig;
}

std::unordered_map<TraderId, int>& RoomStore::ratings(ParticipantTrack track) {
    return track == ParticipantTrack::Bot ? botRatings : manualRatings;
}

bool RoomStore::storeLeaderboardRatings(const CompetitiveLobby& lobby) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (storedRatingLobbies.contains(lobby.id)) {
        return false;
    }

    for (const LeaderboardRow& row : lobby.leaderboardRows()) {
        ratings(row.track)[row.traderId] = row.ratingAfter;
    }

    storedRatingLobbies.insert(lobby.id);
    return true;
}

bool RoomStore::restoreSessionJoin(const PersistedEvent& event) {
    if (event.roomMode == RoomMode::Single) {
        std::lock_guard<std::mutex> lock(sessionMutex);
        SinglePlayerSession* session = ensureSingleSessionLocked(event.roomId, event.traderId, event.createdAt);
        if (session == nullptr) {
            return false;
        }

        session->active = true;
        session->joinedAt = event.createdAt;
        activeByTrader[event.traderId] = {event.sessionId, event.roomId, false};
        cooldownUntilByTrader.erase(event.traderId);
        return true;
    }

    CompetitiveLobby* lobby = findLobby(event.sessionId);
    if (lobby == nullptr) {
        return false;
    }

    const LobbyJoinResult joinResult = lobby->joinAt(event.traderId, event.track, event.createdAt);
    if (joinResult != LobbyJoinResult::Joined && joinResult != LobbyJoinResult::AlreadyJoined) {
        return false;
    }

    std::lock_guard<std::mutex> lock(sessionMutex);
    activeByTrader[event.traderId] = {event.sessionId, event.roomId, true};
    cooldownUntilByTrader.erase(event.traderId);
    return true;
}

bool RoomStore::restoreSessionLeave(const PersistedEvent& event) {
    const TimePoint cooldownUntil = event.cooldownUntil.value_or(
        event.createdAt + std::chrono::seconds(gameConfig.rejoinCooldownSeconds));

    if (event.roomMode == RoomMode::Single) {
        std::lock_guard<std::mutex> lock(sessionMutex);
        SinglePlayerSession* session = ensureSingleSessionLocked(event.roomId, event.traderId, event.createdAt);
        if (session == nullptr) {
            return false;
        }

        cancelTraderOrders(session->game->exchange, event.traderId);
        session->active = false;
        if (activeByTrader.contains(event.traderId) && activeByTrader[event.traderId].id == event.sessionId) {
            markLeftLocked(event.traderId, cooldownUntil);
        } else {
            cooldownUntilByTrader[event.traderId] = cooldownUntil;
        }
        return true;
    }

    CompetitiveLobby* lobby = findLobby(event.sessionId);
    if (lobby == nullptr) {
        return false;
    }

    lobby->leaveAt(event.traderId, event.createdAt);
    cancelTraderOrders(lobby->game->exchange, event.traderId);

    std::lock_guard<std::mutex> lock(sessionMutex);
    if (activeByTrader.contains(event.traderId) && activeByTrader[event.traderId].id == event.sessionId) {
        markLeftLocked(event.traderId, cooldownUntil);
    } else {
        cooldownUntilByTrader[event.traderId] = cooldownUntil;
    }
    return true;
}

GameRoom* RoomStore::gameForPersistedSession(const PersistedEvent& event) {
    if (event.roomMode == RoomMode::Competitive) {
        CompetitiveLobby* lobby = findLobby(event.sessionId);
        return lobby == nullptr ? nullptr : lobby->game.get();
    }

    std::lock_guard<std::mutex> lock(sessionMutex);
    SinglePlayerSession* session = ensureSingleSessionLocked(event.roomId, event.traderId, event.createdAt);
    return session == nullptr ? nullptr : session->game.get();
}

std::unique_ptr<GameRoom> RoomStore::cloneSingleRoom(const GameRoom& room) const {
    auto clone = std::make_unique<GameRoom>();
    clone->id = room.id;
    clone->name = room.name;
    clone->mode = room.mode;
    clone->difficulty = room.difficulty;
    clone->startingCash = room.startingCash;
    clone->maxParticipants = room.maxParticipants;
    clone->startDelaySeconds = room.startDelaySeconds;
    clone->gameDurationSeconds = room.gameDurationSeconds;
    clone->rejoinCooldownSeconds = room.rejoinCooldownSeconds;
    clone->houseLiquidity = room.houseLiquidity;
    clone->assets = room.assets;
    seedRoom(*clone);
    return clone;
}

SinglePlayerSession* RoomStore::ensureSingleSessionLocked(
    const std::string& roomId,
    TraderId traderId,
    TimePoint joinedAt) {
    const GameRoom* room = find(roomId);
    if (room == nullptr || room->mode != RoomMode::Single) {
        return nullptr;
    }

    auto& sessions = singleSessionsByRoom[roomId];
    auto found = sessions.find(traderId);
    if (found == sessions.end()) {
        auto session = std::make_unique<SinglePlayerSession>();
        session->id = roomId + "-" + std::to_string(traderId);
        session->roomId = roomId;
        session->traderId = traderId;
        session->game = cloneSingleRoom(*room);
        session->joinedAt = joinedAt;
        session->active = false;
        found = sessions.emplace(traderId, std::move(session)).first;
    }

    return found->second.get();
}

LobbyJoinOutcome RoomStore::canJoinSessionLocked(TraderId traderId, const std::string& sessionId) const {
    const auto cooldown = cooldownUntilByTrader.find(traderId);
    if (cooldown != cooldownUntilByTrader.end() && cooldown->second > Clock::now()) {
        return makeJoinOutcome(LobbyJoinResult::CoolingDown, secondsRemaining(cooldown->second));
    }

    const auto active = activeByTrader.find(traderId);
    if (active != activeByTrader.end()) {
        if (active->second.id == sessionId) {
            return makeJoinOutcome(LobbyJoinResult::AlreadyJoined);
        }

        return makeJoinOutcome(LobbyJoinResult::ActiveElsewhere, 0, active->second.id);
    }

    return makeJoinOutcome(LobbyJoinResult::Joined);
}

void RoomStore::markJoinedLocked(TraderId traderId, const ActiveSession& session) {
    activeByTrader[traderId] = session;
    cooldownUntilByTrader.erase(traderId);
}

void RoomStore::markLeftLocked(TraderId traderId) {
    markLeftLocked(traderId, Clock::now() + std::chrono::seconds(gameConfig.rejoinCooldownSeconds));
}

void RoomStore::markLeftLocked(TraderId traderId, TimePoint cooldownUntil) {
    activeByTrader.erase(traderId);
    cooldownUntilByTrader[traderId] = cooldownUntil;
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
        GameConfig config = GameConfig::fromEnvironment();
        PersistenceStore persistence;
        RoomStore rooms(config);
        rooms.ratings(ParticipantTrack::Manual) = persistence.loadRatings(ParticipantTrack::Manual);
        rooms.ratings(ParticipantTrack::Bot) = persistence.loadRatings(ParticipantTrack::Bot);
        const RestoreStats restoreStats = restoreFromEvents(rooms, persistence.loadReplayEvents());

        std::cout << "Orderbook API server listening on 0.0.0.0:" << port << "\n";
        std::cout << "Starting cash: " << config.startingCash
                  << ", competitive start delay: " << config.startDelaySeconds
                  << "s, game duration: " << config.gameDurationSeconds
                  << "s, rejoin cooldown: " << config.rejoinCooldownSeconds << "s\n";
        if (persistence.enabled()) {
            std::cout << "Replayed " << restoreStats.events << " persisted events ("
                      << restoreStats.sessions << " session, "
                      << restoreStats.orders << " order, "
                      << restoreStats.cancels << " cancel, "
                      << restoreStats.simulatorTicks << " simulator, "
                      << restoreStats.skipped << " skipped).\n";
        }
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
                sendResponse(client, route(rooms, persistence, *request));
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
