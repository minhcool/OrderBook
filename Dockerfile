FROM gcc:14-bookworm AS build

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends libpq-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

COPY include ./include
COPY src ./src
COPY apps ./apps

RUN g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -pedantic -pthread \
    -static-libstdc++ -static-libgcc \
    -DORDERBOOK_WITH_POSTGRES -I/usr/include/postgresql \
    -Iinclude apps/api_server.cpp src/orderbook.cpp src/exchange.cpp \
    -o /orderbook_api -lpq -lssl -lcrypto

FROM debian:bookworm-slim

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends libpq5 libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /orderbook_api ./orderbook_api

ENV PORT=10000
EXPOSE 10000

CMD ["./orderbook_api"]
