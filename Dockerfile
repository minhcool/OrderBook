FROM gcc:14-bookworm AS build

WORKDIR /app

COPY include ./include
COPY src ./src
COPY apps ./apps

RUN g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -pedantic -pthread \
    -static-libstdc++ -static-libgcc \
    -Iinclude apps/api_server.cpp src/orderbook.cpp src/exchange.cpp \
    -o /orderbook_api

FROM debian:bookworm-slim

WORKDIR /app

COPY --from=build /orderbook_api ./orderbook_api

ENV PORT=10000
EXPOSE 10000

CMD ["./orderbook_api"]
