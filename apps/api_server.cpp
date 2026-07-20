#include "orderbook/exchange.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
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

struct ParsedOrder {
    std::string symbol;
    TraderId traderId = 0;
    OrderId orderId = 0;
    Price price = 0;
    Qty quantity = 0;
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

ParsedOrder parseOrderWithPrice(const std::string& body) {
    ParsedOrder order;

    const auto symbol = jsonStringField(body, "symbol");
    const auto traderId = jsonIntField(body, "traderId");
    const auto orderId = jsonIntField(body, "orderId");
    const auto price = jsonIntField(body, "price");
    const auto quantity = jsonIntField(body, "quantity");

    if (!symbol || !traderId || !orderId || !price || !quantity) {
        throw std::runtime_error("expected symbol, traderId, orderId, price, and quantity");
    }

    order.symbol = *symbol;
    order.traderId = *traderId;
    order.orderId = *orderId;
    order.price = *price;
    order.quantity = *quantity;

    return order;
}

ParsedOrder parseMarketOrder(const std::string& body) {
    ParsedOrder order;

    const auto symbol = jsonStringField(body, "symbol");
    const auto traderId = jsonIntField(body, "traderId");
    const auto orderId = jsonIntField(body, "orderId");
    const auto quantity = jsonIntField(body, "quantity");

    if (!symbol || !traderId || !orderId || !quantity) {
        throw std::runtime_error("expected symbol, traderId, orderId, and quantity");
    }

    order.symbol = *symbol;
    order.traderId = *traderId;
    order.orderId = *orderId;
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

std::optional<std::string> symbolFromBookPath(const std::string& path) {
    const std::string prefix = "/book/";
    if (path.rfind(prefix, 0) != 0 || path.size() <= prefix.size()) {
        return std::nullopt;
    }

    return urlDecode(path.substr(prefix.size()));
}

HttpResponse jsonResponse(int status, const std::string& body) {
    return {status, "application/json", body};
}

HttpResponse handlePostOrder(Exchange& exchange, const std::string& path, const std::string& body) {
    try {
        if (path == "/orders/buy") {
            const ParsedOrder order = parseOrderWithPrice(body);
            return jsonResponse(200, serializeSubmitResult(
                exchange.buy(order.symbol, order.traderId, order.orderId, order.price, order.quantity)));
        }

        if (path == "/orders/sell") {
            const ParsedOrder order = parseOrderWithPrice(body);
            return jsonResponse(200, serializeSubmitResult(
                exchange.sell(order.symbol, order.traderId, order.orderId, order.price, order.quantity)));
        }

        if (path == "/orders/market-buy") {
            const ParsedOrder order = parseMarketOrder(body);
            return jsonResponse(200, serializeSubmitResult(
                exchange.marketBuy(order.symbol, order.traderId, order.orderId, order.quantity)));
        }

        if (path == "/orders/market-sell") {
            const ParsedOrder order = parseMarketOrder(body);
            return jsonResponse(200, serializeSubmitResult(
                exchange.marketSell(order.symbol, order.traderId, order.orderId, order.quantity)));
        }

        if (path == "/orders/ioc-buy") {
            const ParsedOrder order = parseOrderWithPrice(body);
            return jsonResponse(200, serializeSubmitResult(
                exchange.iocBuy(order.symbol, order.traderId, order.orderId, order.price, order.quantity)));
        }

        if (path == "/orders/ioc-sell") {
            const ParsedOrder order = parseOrderWithPrice(body);
            return jsonResponse(200, serializeSubmitResult(
                exchange.iocSell(order.symbol, order.traderId, order.orderId, order.price, order.quantity)));
        }

        if (path == "/orders/fok-buy") {
            const ParsedOrder order = parseOrderWithPrice(body);
            return jsonResponse(200, serializeSubmitResult(
                exchange.fokBuy(order.symbol, order.traderId, order.orderId, order.price, order.quantity)));
        }

        if (path == "/orders/fok-sell") {
            const ParsedOrder order = parseOrderWithPrice(body);
            return jsonResponse(200, serializeSubmitResult(
                exchange.fokSell(order.symbol, order.traderId, order.orderId, order.price, order.quantity)));
        }

        if (path == "/orders/replace-buy") {
            const ParsedOrder order = parseOrderWithPrice(body);
            return jsonResponse(200, serializeSubmitResult(
                exchange.replaceBuy(order.symbol, order.traderId, order.orderId, order.price, order.quantity)));
        }

        if (path == "/orders/replace-sell") {
            const ParsedOrder order = parseOrderWithPrice(body);
            return jsonResponse(200, serializeSubmitResult(
                exchange.replaceSell(order.symbol, order.traderId, order.orderId, order.price, order.quantity)));
        }

        if (path == "/orders/cancel") {
            const auto symbol = jsonStringField(body, "symbol");
            const auto orderId = jsonIntField(body, "orderId");

            if (!symbol || !orderId) {
                throw std::runtime_error("expected symbol and orderId");
            }

            const bool canceled = exchange.cancel(*symbol, *orderId);
            return jsonResponse(200, std::string("{\"canceled\":") + (canceled ? "true" : "false") + "}");
        }
    } catch (const std::exception& ex) {
        return jsonResponse(400, jsonError(ex.what()));
    }

    return jsonResponse(404, jsonError("unknown POST endpoint"));
}

HttpResponse route(Exchange& exchange, const HttpRequest& request) {
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

        const std::optional<std::string> symbol = symbolFromBookPath(path);
        if (symbol) {
            return jsonResponse(200, serializeSnapshot(*symbol, exchange.snapshot(*symbol, parseDepth(query))));
        }

        return jsonResponse(404, jsonError("unknown GET endpoint"));
    }

    if (request.method == "POST") {
        return handlePostOrder(exchange, path, request.body);
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
        << "Access-Control-Allow-Headers: Content-Type\r\n"
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

}  // namespace

int main(int argc, char** argv) {
    int port = 8080;
    if (argc >= 2) {
        port = std::atoi(argv[1]);
    }

    try {
        SocketRuntime runtime;
        SocketHandle server = createServerSocket(port);
        Exchange exchange;

        std::cout << "Orderbook API server listening on http://localhost:" << port << "\n";
        std::cout << "Press Ctrl+C to stop.\n";

        while (true) {
            sockaddr_in clientAddress{};
            int clientAddressSize = sizeof(clientAddress);
            SocketHandle client = accept(server, reinterpret_cast<sockaddr*>(&clientAddress), &clientAddressSize);
            if (client == InvalidSocket) {
                continue;
            }

            std::optional<HttpRequest> request = readRequest(client);
            if (request) {
                sendResponse(client, route(exchange, *request));
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
