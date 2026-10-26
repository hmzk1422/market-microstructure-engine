FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build verilator libssl-dev libpq-dev \
    libboost-dev libboost-system-dev nlohmann-json3-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY hardware/ ./hardware/
COPY exchange/ ./exchange/
RUN cmake -S hardware -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_VERILATED_RTL_TESTS=OFF \
    && cmake --build build --parallel 2 \
    && ctest --test-dir build --output-on-failure

FROM ubuntu:22.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libpq5 libboost-system1.74.0 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r ingestion \
    && useradd -r -g ingestion -d /app -s /usr/sbin/nologin ingestion \
    && mkdir -p /app/logs \
    && chown -R ingestion:ingestion /app

WORKDIR /app/logs
COPY --from=builder /build/build/market_data_engine /app/market_data_engine
USER ingestion

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD ["/app/market_data_engine", "--help"]

ENTRYPOINT ["/app/market_data_engine"]
CMD ["--source", "websocket"]
