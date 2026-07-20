#pragma once

#include "orderbook/constants.h"

class order {
private:
    OrderId id = 0;
    TraderId traderId = 0;
    Price price = 0;
    Qty quantity = 0;
    Qty remainingQuantity = 0;
    Side side = Side::Buy;

public:
    order() = default;

    order(OrderId _id, TraderId _traderId, Price _price, Qty _quantity, Side _side)
        : id(_id),
          traderId(_traderId),
          price(_price),
          quantity(_quantity),
          remainingQuantity(_quantity),
          side(_side) {}

    order(OrderId _id, Price _price, Qty _quantity, Side _side)
        : order(_id, _id, _price, _quantity, _side) {}

    Price getPrice() const {
        return price;
    }

    Qty getQuantity() const {
        return quantity;
    }

    Qty getRemainingQuantity() const {
        return remainingQuantity;
    }

    Side getSide() const {
        return side;
    }

    OrderId getId() const {
        return id;
    }

    TraderId getTraderId() const {
        return traderId;
    }

    void setTraderId(TraderId newTraderId) {
        traderId = newTraderId;
    }

    void setRemainingQuantity(Qty newRemQuant) {
        remainingQuantity = newRemQuant;
    }

    void setQuantity(Qty newQuantity) {
        quantity = newQuantity;
        remainingQuantity = newQuantity;
    }

    void setPrice(Price newPrice) {
        price = newPrice;
    }

    bool isFilled() const {
        return (remainingQuantity == 0);
    }
};
