# ============================================================
# Stage 1: Build
# ============================================================
FROM ubuntu:22.04 AS builder

LABEL description="Market Microstructure Ingestion Engine - Builder"

RUN apt-get update && apt-get install -y \
    build-essential cmake \
    ninja-build \
    libcurl4-openssl-dev \
    libssl-dev \
    libsqlite3-dev \
    nlohmann-json3-dev \
    libspdlog-dev \
    libpq-dev \
    pkg-config \
    git \
    ca-certificates \
    libboost-system-dev \
    && rm -rf /var/lib/apt/lists/*
    
WORKDIR /build

# Copy source
COPY src/ ./src/
COPY CMakeLists.txt ./

# Build the executable (Removed the install target to keep it simple)
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja \
    && cmake --build build

# ============================================================
# Stage 2: Runtime
# ============================================================
FROM ubuntu:22.04 AS runtime

LABEL description="Market Microstructure Ingestion Engine"
LABEL version="1.0.0"

# Install runtime dependencies (Added libboost-system1.74.0)
RUN apt-get update && apt-get install -y \
    libcurl4 \
    libssl3 \
    libsqlite3-0 \
    libpq5 \
    ca-certificates \
    libboost-system1.74.0 \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN groupadd -r ingestion && useradd -r -g ingestion -d /app -s /sbin/nologin ingestion

WORKDIR /app

# Copy built binary directly from the build directory using the correct name
COPY --from=builder /build/build/ws_ingestion /app/ws_ingestion

# (NOTE: Make sure a config.json file actually exists in your project root, 
# otherwise this next line will fail!)
COPY config.json /app/config.json

# Create data and log directories
RUN mkdir -p /app/data /app/logs && \
    chown -R ingestion:ingestion /app

# Switch to non-root user
USER ingestion

# Health check (Updated name to ws_ingestion)
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD /app/ws_ingestion --help > /dev/null 2>&1 || exit 1

# Default command (Updated name to ws_ingestion)
ENTRYPOINT ["/app/ws_ingestion"]
CMD ["-c", "/app/config.json"]

# Expose metrics port
EXPOSE 9090

# Volumes for persistent data
VOLUME ["/app/data", "/app/logs"]