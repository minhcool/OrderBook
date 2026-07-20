#include "orderbook/orderbook.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct BenchmarkResult {
    std::string name;
    std::int64_t operations = 0;
    double milliseconds = 0.0;
};

order makeOrder(OrderId id, TraderId traderId, Price price, Qty quantity, Side side) {
    return order(id, traderId, price, quantity, side);
}

template <typename Fn>
BenchmarkResult runBenchmark(const std::string& name, std::int64_t operations, Fn fn) {
    const auto start = Clock::now();
    fn();
    const auto end = Clock::now();

    const double milliseconds =
        std::chrono::duration<double, std::milli>(end - start).count();

    return {name, operations, milliseconds};
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void seedBuyOrders(orderbook& book, int count, OrderId firstOrderId = 1) {
    for (int i = 0; i < count; ++i) {
        const OrderId orderId = firstOrderId + i;
        const TraderId traderId = 10'000 + i;
        const Price price = 90 + (i % 10);

        SubmitResult result = book.submit(makeOrder(orderId, traderId, price, 1, Side::Buy));
        require(result.accepted, "failed to seed buy order");
    }
}

void seedSellOrders(orderbook& book, int count, OrderId firstOrderId = 1) {
    for (int i = 0; i < count; ++i) {
        const OrderId orderId = firstOrderId + i;
        const TraderId traderId = 20'000 + i;
        const Price price = 100 + (i % 5);

        SubmitResult result = book.submit(makeOrder(orderId, traderId, price, 1, Side::Sell));
        require(result.accepted, "failed to seed sell order");
    }
}

BenchmarkResult benchmarkRestingLimitSubmits() {
    constexpr int orderCount = 10'000;
    orderbook book;

    BenchmarkResult result = runBenchmark("resting limit submits", orderCount, [&]() {
        seedBuyOrders(book, orderCount);
    });

    require(book.totalBuyQuantity() == orderCount, "resting submit quantity mismatch");
    return result;
}

BenchmarkResult benchmarkCrossingLimitMatches() {
    constexpr int orderCount = 10'000;
    orderbook book;

    seedSellOrders(book, orderCount);

    BenchmarkResult result = runBenchmark("crossing limit matches", orderCount, [&]() {
        SubmitResult submitResult =
            book.submit(makeOrder(1'000'000, 999'999, 104, orderCount, Side::Buy));

        require(submitResult.accepted, "crossing order was rejected");
        require(submitResult.filledQuantity == orderCount, "crossing fill quantity mismatch");
        require(submitResult.trades.size() == static_cast<std::size_t>(orderCount),
                "crossing trade count mismatch");
    });

    require(book.empty(), "book should be empty after crossing match");
    return result;
}

BenchmarkResult benchmarkCancelById() {
    constexpr int orderCount = 5'000;
    orderbook book;

    seedBuyOrders(book, orderCount);

    BenchmarkResult result = runBenchmark("cancel by id", orderCount, [&]() {
        for (int i = orderCount - 1; i >= 0; --i) {
            require(book.cancel(1 + i), "cancel failed");
        }
    });

    require(book.empty(), "book should be empty after cancels");
    return result;
}

BenchmarkResult benchmarkReplaceExistingOrders() {
    constexpr int orderCount = 5'000;
    orderbook book;

    seedBuyOrders(book, orderCount);

    BenchmarkResult result = runBenchmark("replace existing orders", orderCount, [&]() {
        for (int i = 0; i < orderCount; ++i) {
            const OrderId orderId = 1 + i;
            const TraderId traderId = 10'000 + i;
            const Price newPrice = 80 + (i % 10);

            SubmitResult replaceResult =
                book.replace(makeOrder(orderId, traderId, newPrice, 2, Side::Buy));

            require(replaceResult.accepted, "replace failed");
            require(replaceResult.restingQuantity == 2, "replacement did not rest expected quantity");
        }
    });

    require(book.totalBuyQuantity() == orderCount * 2, "replace quantity mismatch");
    return result;
}

BenchmarkResult benchmarkConcurrentSubmits() {
    constexpr int threadCount = 4;
    constexpr int ordersPerThread = 2'000;
    constexpr int totalOrders = threadCount * ordersPerThread;

    orderbook book;

    BenchmarkResult result = runBenchmark("concurrent submits", totalOrders, [&]() {
        std::vector<std::thread> threads;
        threads.reserve(threadCount);

        for (int threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
            threads.emplace_back([&book, threadIndex]() {
                for (int i = 0; i < ordersPerThread; ++i) {
                    const OrderId orderId = 1 + threadIndex * ordersPerThread + i;
                    const TraderId traderId = 30'000 + threadIndex;
                    const Price price = 70 + threadIndex;

                    SubmitResult submitResult =
                        book.submit(makeOrder(orderId, traderId, price, 1, Side::Buy));

                    require(submitResult.accepted, "concurrent submit failed");
                }
            });
        }

        for (std::thread& thread : threads) {
            thread.join();
        }
    });

    require(book.totalBuyQuantity() == totalOrders, "concurrent submit quantity mismatch");
    return result;
}

void printResult(const BenchmarkResult& result) {
    const double opsPerSecond = result.operations / (result.milliseconds / 1000.0);
    const double microsecondsPerOp = (result.milliseconds * 1000.0) / result.operations;

    std::cout << std::left << std::setw(28) << result.name
              << std::right << std::setw(12) << result.operations
              << std::setw(14) << std::fixed << std::setprecision(2) << result.milliseconds
              << std::setw(16) << std::fixed << std::setprecision(0) << opsPerSecond
              << std::setw(14) << std::fixed << std::setprecision(3) << microsecondsPerOp
              << "\n";
}

}  // namespace

int main() {
    try {
        std::vector<BenchmarkResult> results;
        results.push_back(benchmarkRestingLimitSubmits());
        results.push_back(benchmarkCrossingLimitMatches());
        results.push_back(benchmarkCancelById());
        results.push_back(benchmarkReplaceExistingOrders());
        results.push_back(benchmarkConcurrentSubmits());

        std::cout << "Orderbook performance benchmarks\n";
        std::cout << "Lower us/op and higher ops/sec are better. Compare runs on the same machine.\n\n";
        std::cout << std::left << std::setw(28) << "scenario"
                  << std::right << std::setw(12) << "ops"
                  << std::setw(14) << "ms"
                  << std::setw(16) << "ops/sec"
                  << std::setw(14) << "us/op"
                  << "\n";
        std::cout << std::string(84, '-') << "\n";

        for (const BenchmarkResult& result : results) {
            printResult(result);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Performance benchmark failed: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
