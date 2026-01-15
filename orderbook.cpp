#include "constants.h"
#include "order.h"
#include "map"
#include "set"
#include "queue"
#include <cassert>
#include <utility>
#include <map>
#include <iomanip>

class orderbook{
    /*
    The "buys" map maps the price to the index in the buyQueues
    The "availableBuyIndexes" shows the indexes that are available (all of the offers that has the
    old price that corresponds to the index has been done), so that we can use the index in another occasion
    The "buyQueues" store the buys/sells offers of same price, sorted by id (we assume that the later a
    buy/sell offer is, the higher its id)
    */
    std::map<int, int, std::greater<int>> buys;
    std::set<int> availableBuyIndexes;
    std::vector<std::set<order>> buyQueues;

    // the sell side does the same
    std::map<int, int, std::greater<int>> sells;
    std::set<int> availableSellIndexes;
    std::vector<std::set<order>> sellQueues;
    /*
    Inserting a new price into a certain index
    */
    void insert(int price, int index, Side side){
        if(side == Side::Buy){
            availableBuyIndexes.erase(index);
            buys[price] = index;
            if(buyQueues.size() <= index){
                std::set<order> newOrder;
                buyQueues.push_back(newOrder);
            }
        }
        else{
            availableSellIndexes.erase(index);
            sells[price] = index;
            if(sellQueues.size() <= index){
                std::set<order> newOrder;
                sellQueues.push_back(newOrder);
            }
        }
    }
    // erase a certain price (because all of its offers has been executed)
    void erase(int price, Side side){
        if(side == Side::Buy){
            int index = buys[price];
            availableBuyIndexes.insert(index);
            buys.erase(price);
        }
        else{
            int index = sells[price];
            availableSellIndexes.insert(index);
            sells.erase(price);
        }
    }
    /*
    GetIndex of some price (we assume that there is at least one offer that corresponds to this price if it is called)
    */
    int getIndex(int price, Side side){
        if(side == Side::Buy){
            if(buys.find(price) != buys.end()) return buys[price];
            else{
                if(availableBuyIndexes.empty()){
                    int index = buyQueues.size();
                    insert(price, index, side);
                    return index;
                }
            }
        }
        else{
            if(sells.find(price) != sells.end()) return sells[price];
            else{
                if(availableSellIndexes.empty()){
                    int index = sellQueues.size();
                    insert(price, index, side);
                    return index;
                }
            }
        }
    }
    // simulator of trade result (in case of FoK, we will need this)
    std::pair<int, int> TradeResult(order order){
        if(order.getSide() == Side::Buy){
            std::pair<int, int> answer = {0, 0};
            int remainingQuant = order.getQuantity();
            for(auto sell_prices : sells){
                int price = sell_prices.first, index = sell_prices.second;
                if(price > order.getPrice()) break;
                for(auto offers : sellQueues[index]){
                    int currentQuantity = offers.getQuantity();
                    if(currentQuantity >= remainingQuant){
                        answer.first += remainingQuant;
                        answer.second += remainingQuant * price;
                        remainingQuant = 0;
                        break;
                    }
                    else{
                        answer.first += currentQuantity;
                        answer.second += currentQuantity * price;
                        remainingQuant -= currentQuantity;
                    }
                }
                if(remainingQuant <= 0) break;
            }
            return answer;
        }
        else{
            std::pair<int, int> answer = {0, 0};
            int remainingQuant = order.getQuantity();
            for(auto buy_prices : buys){
                int price = buy_prices.first, index = buy_prices.second;
                if(price < order.getPrice()) break;
                for(auto offers : buyQueues[index]){
                    int currentQuantity = offers.getQuantity();
                    if(currentQuantity >= remainingQuant){
                        answer.first += remainingQuant;
                        answer.second += remainingQuant * price;
                        remainingQuant = 0;
                        break;
                    }
                    else{
                        answer.first += currentQuantity;
                        answer.second += currentQuantity * price;
                        remainingQuant -= currentQuantity;
                    }
                }
                if(remainingQuant <= 0) break;
            }
            return answer;
        }
    }
    void executeTrade(order order){
        if(order.getSide() == Side::Buy){
            std::pair<int, int> answer = {0, 0};
            int remainingQuant = order.getQuantity();
            for(auto sell_prices : sells){
                int price = sell_prices.first, index = sell_prices.second;
                if(price > order.getPrice()) break;
                std::set<order>::iterator it = sellQueues[index].begin(), it2 = it;
                order lastOrder;// for inserting back into the sellQueue
                for(; it2 != sellQueues[it].begin(); it2++){
                    order offers = (*it2);
                    lastOrder = offers;
                    int currentQuantity = offers.getQuantity();
                    if(currentQuantity >= remainingQuant){
                        lastOrder.setRemainingQuantity(currentQuantity - remainingQuant);
                        remainingQuant = 0;
                        it2++;// erase doesn't include the current one, but we want to also erase the current one
                        break;
                    }
                    else{
                        remainingQuant -= currentQuantity;
                    }
                }
                sellQueues[index].erase(it, it2);
                sellQueues[index].insert(lastOrder);
                if(remainingQuant <= 0) break;
            }
        }
        else{
            std::pair<int, int> answer = make_pair(0, 0);
            int remainingQuant = order.getQuantity();
            for(auto buy_prices : buys){
                int price = buy_prices.first, index = buy_prices.second;
                if(price < order.getPrice()) break;
                set<order>::iterator it = buyQueues[index].begin(), it2 = it;
                order lastOrder;// for inserting back into the buyQueue
                for(; it2 != buyQueues[it].begin(); it2++){
                    order offers = (*it2);
                    lastOrder = offers;
                    int currentQuantity = offers.getQuantity();
                    if(currentQuantity >= remainingQuant){
                        lastOrder.setRemainingQuantity(currentQuantity - remainingQuant);
                        remainingQuant = 0;
                        it2++;// erase doesn't include the current one, but we want to also erase the current one
                        break;
                    }
                    else{
                        remainingQuant -= currentQuantity;
                    }
                }
                buyQueues[index].erase(it, it2);
                buyQueues[index].insert(lastOrder);
                if(remainingQuant <= 0) break;
            }
        }
    }
    void addOrder(order order){
        int index = getIndex(order.getPrice(), order.getSide());
        if(order.getSide() == Side::Buy){
            buyQueues[index].insert(order);
        }
        else{
            sellQueues[index].insert(order);
        }
    }
    // return 1 if add successfully
    bool add(order order, Type type){
        assert(side == Side::Buy || side == Side::Sell);
        if(order.getQuantity() <= 0){
            std::cout << "WRONG QUANTITY - QUANTITY CAN'T BE NON-POSITIVE\n";
            return 0;
        }
        if(order.getPrice() <= 0){
            std::cout << "WRONG PRICE - PRICE CAN'T BE NON-POSITIVE\n";
            return 0;
        }
        if(type == Type::Regular){
            std::pair<int, int> trade = TradeResult(order);
            std::cout << "SUCCESSFULLY TRADED " << trade.first << " OUT OF " << order.getQuantity() << " WITH AN AVERAGE PRICE OF ";
            std::cout << std::fixed << std::setprecision(2) << (double)trade.second / (double)trade.first << "\n";
            executeTrade(order);
            order.setRemainingQuantity(order.getQuantity() - trade.first);    
            addOrder(order);
        }
        else if(type == Type::Market){
            order.setPrice(Constants::MAXPRICE);
            std::pair<int, int> trade = TradeResult(order);
            std::cout << "SUCCESSFULLY TRADED " << trade.first << " OUT OF " << order.getQuantity() << " WITH AN AVERAGE PRICE OF ";
            std::cout << std::fixed << std::setprecision(2) << (double)trade.second / (double)trade.first << "\n";
            executeTrade(order);
            order.setRemainingQuantity(order.getQuantity() - trade.first);    
            addOrder(order);
        }
        else if(type == Type::IoC){
            std::pair<int, int> trade = TradeResult(order);
            std::cout << "SUCCESSFULLY TRADED " << trade.first << " OUT OF " << order.getQuantity() << " WITH AN AVERAGE PRICE OF ";
            std::cout << std::fixed << std::setprecision(2) << (double)trade.second / (double)trade.first << "\n";
            std::cout << "DISCARD THE REST\n";
            executeTrade(order);
        }
        else if(type == Type::FoK){
            std::pair<int, int> trade = TradeResult(order);
            if(trade.first == order.getQuantity()){
                std::cout << "SUCCESSFULLY TRADED " << trade.first << " OUT OF " << order.getQuantity() << " WITH AN AVERAGE PRICE OF ";
                std::cout << std::fixed << std::setprecision(2) << (double)trade.second / (double)trade.first << "\n";
                executeTrade(order);
            }
            else{
                std::cout << "UNSUCCESSFULLY TRADE\n";
            }
        }
        else{
            std::cout << "WRONG TYPE\n";
            return 0;
        }
    }
    void delBuy(order order){
        int index = getIndex(order.getPrice(), order.getSide());
        buyQueue[index].erase(order);
        if(buyQueue[index].size() == 0){
            erase(order.getPrice(), Side::Buy);
        }
    }
    void delSell(order order){
        int index = getIndex(order.getPrice(), order.getSide());
        sellQueue[index].erase(order);
        if(SellQueue[index].size() == 0){
            erase(order.getPrice(), Side::Sell);
        }
    }
    void changeOrder(order order, Qty newQuant, Price newPrice, Side side){
        assert(side == Side::Buy || side == Side::Sell);
        delBuy(order);
        order.setQuantity(newQuant);
        order.setPrice(newPrice);
        addOrder(order);
    }
    
};