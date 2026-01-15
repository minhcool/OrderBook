#include "constants.h"
#include<iostream>
#include<memory>
#include<set>
#include<map>

class order{
private:
    OrderId id;
    Price price;// in cents
    Qty quantity, remainingQuantity;
    Side side;
public:
    order(){}
    order(int _id, int _price, int _quantity, Side _side){
        id = _id;
        price = _price;
        quantity = _quantity;
        side = _side;
        remainingQuantity = quantity;
    }
    // the orders can't be modi
    Price getPrice(){
        return price;
    }
    Qty getQuantity(){
        return quantity;
    }
    Qty getRemainingQuantity(){
        return remainingQuantity;
    }
    Side getSide(){
        return side;
    }
    OrderId getId(){
        return id;
    }
    void setRemainingQuantity(int newRemQuant){
        remainingQuantity = newRemQuant;
    }
    void setQuantity(int newQuantity){
        quantity = newQuantity;
    }
    void setPrice(int newPrice){
        price = newPrice;
    }
    bool isFilled(){
        return (remainingQuantity == 0);
    }
};