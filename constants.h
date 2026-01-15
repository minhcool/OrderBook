#include <cstdint>

namespace Constants {
    inline constexpr int MAXPRICE = 1e9;
}

using OrderId = int;
using Price   = int;
using Qty     = int;

enum class Side {
    Buy,
    Sell
};

enum class Type{
    Regular,
    Market,
    IoC, 
    FoK,
}