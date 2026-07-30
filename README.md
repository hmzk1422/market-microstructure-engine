# Market Microstructure Ingestion Engine

Professional-grade C++ ingestion engine for real-time market data from **Bybit WebSocket** streams, with SQLite persistence, data validation, order book reconstruction, and robust error recovery.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      Bybit WebSocket V5                         │
│              wss://stream.bybit.com/v5/public/linear             │
└──────────────────────────┬──────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────────┐
│                    BybitWsClient                                 │
│  • Connection management with exponential backoff reconnection   │
│  • HMAC-SHA256 authentication for private streams                │
│  • Subscription management (orderbook, trades, ticker, kline)    │
│  • Sequence gap detection                                        │
│  • Message parsing & routing                                     │
└──────────────────────────┬──────────────────────────────────────┘
                           │  IngestedRecord (shared_ptr)
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                      DataPipeline                                │
│                                                                  │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐       │
│  │  Ingestion   │───▶│  Processing  │───▶│   Storage    │       │
│  │   Thread     │    │   Thread     │    │   Thread     │       │
│  │ (monitoring) │    │ (validation) │    │  (DB batch)  │       │
│  └──────────────┘    │  + sequencing│    └──────┬───────┘       │
│                      └──────────────┘           │               │
│                                                  ▼               │
│                                         ┌────────────────┐      │
│                                         │   SQLite (WAL)  │      │
│                                         │  Connection Pool│      │
│                                         └────────────────┘      │
└─────────────────────────────────────────────────────────────────┘
```

## Features

### Data Ingestion
- **Bybit V5 WebSocket** — connects to public/private streams
- **Multiple data types** — order book (snapshot + delta), trades, tickers, klines
- **Configurable subscriptions** — choose symbols, depth levels, kline intervals
- **HMAC-SHA256 authentication** — secure private stream access

### Data Integrity
- **Checksum verification** — SHA-256 based integrity check on every record
- **Data validation** — price/size sanity checks, OHLC consistency, timestamp validation
- **Sequence gap detection** — monotonic sequence tracking per symbol
- **Order book reconstruction** — snapshot + delta sequencing with gap recovery
- **Crossed book detection** — warns when best bid >= best ask

### Error Management
- **Exponential backoff with jitter** — smart reconnection strategy
- **Unlimited reconnection** — configurable max attempts
- **Graceful shutdown** — signal handler (SIGINT/SIGTERM) flushes all pending data
- **Queue overflow protection** — bounded queues with backpressure
- **Audit trail** — all raw messages stored for replay/debugging

### Performance
- **Multi-threaded pipeline** — ingestion, processing, storage run on separate threads
- **SQLite WAL mode** — concurrent reads/writes without locking
- **Connection pooling** — 4 parallel DB connections (configurable)
- **Batch writes** — transactional batch inserts (1000 records/batch)
- **Async logging** — spdlog with rotating file sink
- **Memory-mapped I/O** — SQLite mmap for fast reads

### Security
- **API keys via environment variables** — never hardcoded
- **SSL verification** — libcurl with peer/host verification
- **Non-root Docker** — runs as `ingestion` user
- **Checksum verification** — data tampering detection

## Project Structure

```
├── config.json                    # Default configuration
├── .env.example                   # Environment variable template
├── Dockerfile                     # Multi-stage Docker build
├── docker-compose.yml             # Docker Compose setup
├── CMakeLists.txt                 # CMake build system
├── src/
│   └── ingestion/
│       ├── main.cpp               # Entry point with signal handling
│       ├── config.h               # Configuration (JSON + env vars)
│       ├── models/
│       │   └── market_data.h      # Data models (Trade, OrderBook, Ticker, Kline)
│       ├── websocket/
│       │   ├── ws_session.h/.cpp  # Low-level libcurl WebSocket session
│       │   └── bybit_ws_client.h/.cpp  # Bybit-specific client
│       ├── pipeline/
│       │   ├── data_pipeline.h/.cpp  # Main pipeline orchestrator
│       │   ├── validator.h/.cpp      # Data validation
│       │   └── sequencer.h/.cpp      # Order book reconstruction
│       ├── storage/
│       │   ├── schema.h           # SQL table definitions
│       │   └── db_manager.h/.cpp  # SQLite connection pool
│       └── utils/
│           ├── logger.h/.cpp      # Async logging (spdlog)
│           ├── backoff.h          # Exponential backoff with jitter
│           └── crypto.h           # HMAC-SHA256, SHA-256, checksums
└── data/                          # SQLite database location
└── logs/                          # Log file location
```

## Prerequisites

### System Dependencies
- **C++20** compiler (GCC 11+, Clang 14+, MSVC 2022+)
- **CMake** 3.20+
- **OpenSSL** 1.1+ (libssl-dev)
- **libcurl** 7.80+ (with WebSocket support)
- **SQLite3** 3.35+
- **nlohmann-json** 3.10+
- **spdlog** 1.10+

### Install Dependencies

**Ubuntu/Debian:**
```bash
sudo apt-get install -y \
    build-essential cmake ninja-build \
    libcurl4-openssl-dev libssl-dev \
    libsqlite3-dev nlohmann-json3-dev \
    libspdlog-dev pkg-config
```

**macOS (Homebrew):**
```bash
brew install cmake ninja openssl curl sqlite3 nlohmann-json spdlog
```

**Windows (vcpkg):**
```bash
vcpkg install curl[openssl] openssl sqlite3 nlohmann-json spdlog
```

## Build & Run

### Native Build
```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --parallel

# Run
./build/bybit_ingestion -c config.json
```

### Docker Build
```bash
# Build image
docker build -t market-ingestion .

# Run with environment variables
docker run -d \
    --name market-ingestion \
    -e BYBIT_API_KEY=your_key \
    -e BYBIT_API_SECRET=your_secret \
    -v $(pwd)/data:/app/data \
    -v $(pwd)/logs:/app/logs \
    -p 9090:9090 \
    market-ingestion
```

### Docker Compose
```bash
# Create .env file from template
cp .env.example .env
# Edit .env with your API keys

# Start (2nd for hard reset)
docker-compose up -d
docker-compose up -d --force-recreate ingestion

# View logs
docker-compose logs -f

# Stop
docker-compose down

# Check Container Status
docker compose ps

# View Live Logs
docker compose logs -f ingestion

# Rebuild
docker compose build ingestion

# All in one
docker compose up -d --build --force-recreate ingestion
```

### Simulator
```
# 1. Run static analysis (Phase 1)
python research/liquidity_capacity.py

# 2. Run dynamic simulation (Phase 2)
python research/liquidity_capacity_dynamic.py

# 3. Open the notebook for visualization
jupyter notebook research/liquidity_capacity_analysis.ipynb
```

## Configuration

### config.json
All settings are in `config.json`. Key sections:

| Section | Key | Default | Description |
|---------|-----|---------|-------------|
| Connection | `bybit_ws_url` | `wss://stream.bybit.com/v5/public/linear` | WebSocket endpoint |
| Subscriptions | `symbols` | `["BTCUSDT", "ETHUSDT"]` | Trading pairs |
| Subscriptions | `orderbook_depth` | `50` | Depth levels (1, 50, 200, 500) |
| WebSocket | `ws_ping_interval_sec` | `20` | Ping interval |
| WebSocket | `ws_max_reconnect_attempts` | `0` | 0 = unlimited |
| Pipeline | `pipeline_queue_size` | `100000` | Max queued records |
| Pipeline | `pipeline_batch_size` | `1000` | DB batch size |
| Database | `db_path` | `data/market_data.db` | SQLite file path |
| Database | `db_journal_mode` | `WAL` | Write-Ahead Logging |
| Logging | `log_level` | `info` | trace/debug/info/warn/error |

### Environment Variables
Override config values at runtime:

| Variable | Description |
|----------|-------------|
| `BYBIT_API_KEY` | Bybit API key |
| `BYBIT_API_SECRET` | Bybit API secret |
| `INGESTION_DB_PATH` | Database file path |
| `INGESTION_LOG_LEVEL` | Log level |
| `INGESTION_LOG_DIR` | Log directory |
| `INGESTION_WS_URL` | WebSocket URL override |
| `INGESTION_SYMBOLS` | Comma-separated symbols |

## Database Schema

### Tables
| Table | Description | Key Constraints |
|-------|-------------|-----------------|
| `trades` | Trade data | UNIQUE(symbol, trade_id) |
| `orderbook_snapshots` | Full order book snapshots | UNIQUE(symbol, sequence_number) |
| `orderbook_deltas` | Incremental order book updates | UNIQUE(symbol, sequence_number) |
| `tickers` | 24hr ticker statistics | — |
| `klines` | Candlestick data | UNIQUE(symbol, interval, start_time_us) |
| `raw_messages` | Audit trail of all WebSocket messages | — |
| `pipeline_metadata` | Pipeline state tracking | PRIMARY KEY(key) |

### Indexes
- `trades(symbol, timestamp_us)` — time-series queries
- `orderbook_snapshots(symbol, sequence_number)` — order book replay
- `orderbook_deltas(symbol, sequence_number)` — delta application
- `tickers(symbol, timestamp_us)` — ticker history
- `klines(symbol, interval, start_time_us)` — kline queries
- `raw_messages(received_at_us)` — audit trail queries

## Error Recovery

### WebSocket Disconnection
1. Exponential backoff starts (1s → 2s → 4s → ... → 60s max)
2. Random jitter (±25%) prevents thundering herd
3. On reconnect, all subscriptions are re-issued
4. Order book state is rebuilt from fresh snapshot

### Sequence Gaps
1. Gap detected when delta sequence is non-monotonic
2. Pending deltas are queued until next snapshot
3. Snapshot replaces entire book state
4. Queued deltas with sequence > snapshot are applied

### Data Validation Failures
1. Invalid records are logged with full context
2. Records are dropped from the pipeline
3. Validation statistics track pass/fail rates
4. Configurable checksum verification

## Performance Tuning

### Database
- **WAL mode** — enables concurrent reads during writes
- **Connection pool** — increase `db_pool_size` for higher throughput
- **Batch size** — adjust `pipeline_batch_size` (500-5000)
- **Cache size** — SQLite cache is set to 64MB by default

### Pipeline
- **Queue size** — increase `pipeline_queue_size` for burst absorption
- **Flush interval** — decrease `pipeline_flush_interval_ms` for lower latency
- **Thread model** — 3 dedicated threads (ingestion, processing, storage)

### Network
- **Ping interval** — adjust `ws_ping_interval_sec` for connection health
- **TCP keepalive** — enabled with 60s idle, 30s interval

## Monitoring

### Logs
- **Console** — color-coded output with timestamps and thread IDs
- **File** — rotating files (100MB each, 10 files max) with source locations
- **Structured** — consistent format for log parsing

### Statistics (logged every 60s)
```
Pipeline stats | received=150000 | passed=149850 | failed=150 | stored=149800 | errors=0 | qsize=234
DB stats | trades=50000 | snaps=100 | deltas=50000 | tickers=50000 | klines=50000 | size=256MB
```

### Health Check
- Docker HEALTHCHECK runs every 30s
- Database integrity verified with `SELECT 1`
- WebSocket state monitored (CONNECTED/CONNECTING/FAILED)

## License

MIT License — see LICENSE file for details.

@article{berti2025trades,
  title={TRADES: Generating Realistic Market Simulations with Diffusion Models},
  author={Berti, Leonardo and Prenkaj, Bardh and Velardi, Paola},
  journal={arXiv preprint arXiv:2502.07071},
  year={2025}
}