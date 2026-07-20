#pragma once

#include <cstdint>

using OrderId = std::int64_t;
using TraderId = std::int64_t;
using Price = std::int64_t;
using Qty = std::int64_t;

namespace Constants {
inline constexpr Price MAXPRICE = 1'000'000'000;
}

enum class Side {
    Buy,
    Sell
};

enum class Type {
    Regular,
    Market,
    IoC,
    FoK,
};
