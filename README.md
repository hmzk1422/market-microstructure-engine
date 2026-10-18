# Market Microstructure Ingestion Engine PoW
Professional-grade C++ pipeline for real-time Bybit WebSocket ingestion, coupled with a Verilated SystemVerilog hardware accelerator for ultra-low-latency spread detection, and a Python-based quantitative research simulator.

## Architecture
```mermaid
flowchart TD
    subgraph Python ["Python Research & DeepMarket Simulator"]
        direction LR
        A["Python Research &<br>DeepMarket Simulator"] -->|"1. Find parameters<br>(e.g., Threshold = 50)"| B["SystemVerilog RTL<br>(spread_detector.sv)"]
    end

    subgraph Hardware ["Hardware Acceleration"]
        direction TB
        B -->|"2. Enforced at hardware speed<br>on live exchange data"| C["C++ Ingestion &<br>Verilated Pipeline"]
    end
```
Flow:  
```mermaid
sequenceDiagram
    autonumber
    participant Exchange as Fiber Exchange
    participant Pipeline as src/pipeline (C++/Rust TCP/IP)
    participant Parser as hardware/rtl (Packet Parser)
    participant Strategy as src/strategy (Risk Limits/Computations)
    participant Exec as hardware/rtl (Execution Engine)

    Exchange->>Pipeline: Raw Ethernet/TCP Frames (Market Data Feed)
    Note over Pipeline,Parser: Low-level network unpacking
    Pipeline->>Parser: Streaming Byte-Stream (rx_data + rx_valid)
    
    Note over Parser,Strategy: Hot Path (RTL Execution)
    Parser->>Strategy: Parsed LOB State & Price Ticks (32-bit Prices, Asset IDs)
    Strategy->>Exec: Arbitrage Evaluation & Spread Trigger (Valid Signal + Payload Struct)
    
    Note over Exec,Pipeline: Outbound Execution Loop
    Exec->>Pipeline: Raw Execution Command (Order Payload: Side, Volume, Price)
    Pipeline->>Exchange: Outformed TCP/IP Packets -> Fiber Cables
```
## Core Commands/References
Run using WSL/Linux.
### C++ Pipeline & Hardware Build
```bash
# Clear old build artifacts and configure for Linux
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build both the ingestion engine and Verilator hardware testbench
cmake --build build --parallel

# Run the standalone hardware simulation testbench
./build/hardware/sim/sim_spread_detector

# Run the live ingestion pipeline
./build/bybit_ingestion -c config.json
```
### Quantitative Simulator & Research (Python/ABIDES)
```bash
# Navigate to the simulator workspace
cd simulator/DeepMarket/ABIDES

# Run a clean independent crypto limit order simulation test
python config/crypto_limit_order_sim.py --qty 0.5

# Run with a larger event slice
python config/crypto_limit_order_sim.py --event-limit 1000000
```
### Docker Deployment
```bash
# Start via Docker Compose
cp .env.example .env
docker compose up -d --build --force-recreate ingestion

# View live execution logs
docker compose logs -f ingestion

# Stop containers
docker compose down
```

# Citations
```
@article{berti2025trades,
  title={TRADES: Generating Realistic Market Simulations with Diffusion Models},
  author={Berti, Leonardo and Prenkaj, Bardh and Velardi, Paola},
  journal={arXiv preprint arXiv:2502.07071},
  year={2025}
}
```
