"""
exchange/abides/exchange_simulator.py
=====================================

Python ABIDES-based exchange simulator that exposes a live L2 order book over a
Unix domain socket to the C++ application.

Architecture
------------
  +------------------------+        UDS (binary packets)        +------------------+
  |   ABIDES (Python)      |  ------------------------->      |   C++ Application|
  |   (simulates L2 Order  |                                  |   (reads socket, |
  |    Book)               |                                  |    computes spread|
  +------------------------+                                  +------------------+
           |                                                              |
           |  (ABIDES events)                                             |  (Verilator RTL)
           |                                                              v
           |                                                 +------------------+
           +------------------------------------------------>>   spread_detector  |
                                                              |   (250 MHz RTL)  |
                                                              +------------------+

Protocol (binary, little-endian):
  [0:4]     magic   : uint32  0x4D4B5254 ("MKRT" = "Market Kernel")
  [4:8]     payload_len : uint32  length of payload after 8-byte header
  [8:12]    msg_type: uint32
              1 = SNAPSHOT   (full orderbook)
              2 = DELTA      (partial update)
              3 = HEARTBEAT  (keep-alive)
  [12:20]   timestamp_ns : uint64
  [20:24]   num_symbols  : uint32
  For each symbol:
    [..] symbol_name : C-string (padded to 16 bytes)
    [..] num_levels  : uint32
    for each level:
      price  : int64  (price * 1e8)
      size   : int64  (size * 1e8)
"""

import argparse
import os
import platform
import random
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional


# ---------------------------------------------------------------------------
# Binary packet helpers (little-endian = x86-64 compatible)
# ---------------------------------------------------------------------------

def _pack_u32(v: int) -> bytes:
    return struct.pack("<I", v & 0xFFFFFFFF)


def _pack_u64(v: int) -> bytes:
    return struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF)


class AbidesExchangeSimulator:
    """
    Simulates an exchange's L2 order book in Python (in the style of ABIDES's
    ExchangeAgent) and pushes snapshots/heartbeats over a Unix domain socket.
    """

    def __init__(self, socket_path: str, symbols: List[str],
                 depth: int = 20, scale: float = 1e8,
                 snapshot_interval_ns: int = 4000,  # 4us = 250kHz snapshots
                 host: str = "127.0.0.1", port: int = 6789,
                 use_tcp: bool = False):
        self.socket_path = socket_path
        self.symbols = symbols
        self.depth = depth
        self.scale = scale
        self.snapshot_interval_ns = snapshot_interval_ns
        self.books = {s: OrderBook(s, depth=depth, scale=scale) for s in symbols}
        self._server: Optional[socket.socket] = None
        self._tcp_server: Optional[socket.socket] = None
        self._tcp_port = port
        self._use_tcp = use_tcp or platform.system() == "Windows"
        self._clients: Dict[int, socket.socket] = {}
        self._next_id = 0
        self._running = False
        self._last_send = 0.0

    # -- socket management -------------------------------------------------
    def _ensure_server(self):
        if self._server is None and not self._use_tcp:
            self._server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._server.bind(self.socket_path)
            self._server.listen(16)
            self._server.settimeout(0.1)

        if self._tcp_server is None and self._use_tcp:
            self._tcp_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._tcp_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self._tcp_server.bind(("127.0.0.1", self._tcp_port))
            self._tcp_server.listen(16)
            self._tcp_server.settimeout(0.1)
            print(f"[ABIDES] TCP server listening on 127.0.0.1:{self._tcp_port}")

    def _connect_client(self) -> Optional[socket.socket]:
        try:
            if self._use_tcp and self._tcp_server:
                conn, addr = self._tcp_server.accept()
            elif self._server:
                conn, addr = self._server.accept()
            else:
                return None
            conn.settimeout(5.0)
            self._clients[self._next_id] = conn
            cid = self._next_id
            self._next_id += 1
            return conn if cid == 0 else None
        except socket.timeout:
            return None

    def _send(self, conn: socket.socket, packet: bytes):
        conn.sendall(packet)

    # -- simulation --------------------------------------------------------
    def _seed_books(self):
        """Create initial market state so spread is non-zero immediately."""
        bases = {"BTCUSDT": 60000.0, "ETHUSDT": 3000.0, "SOLUSDT": 100.0}
        for sym, book in self.books.items():
            base = bases.get(sym, float(len(sym)) * 100.0)
            for i in range(self.depth):
                bid_p = base - (i + 1) * 5.0
                ask_p = base + (i + 1) * 5.0
                book.bids[bid_p] = 10.0 - i * 0.1
                book.asks[ask_p] = 10.0 - i * 0.1

    def _run_loop(self):
        while self._running:
            self._advance_market()
            timestamp_ns = int(time.perf_counter_ns())
            now = time.perf_counter()
            if now - self._last_send >= self.snapshot_interval_ns / 1e9:
                levels = {sym: [] for sym in self.symbols}
                best_bids = {}
                best_asks = {}
                spreads = {}
                for sym, book in self.books.items():
                    bid_levels = sorted(book.bids.items(), reverse=True)[:3]
                    ask_levels = sorted(book.asks.items())[:3]
                    levels[sym] = bid_levels + ask_levels
                    best_bids[sym] = book.best_bid()
                    best_asks[sym] = book.best_ask()
                    spreads[sym] = book.spread()
                packet = build_packet(1, timestamp_ns, self.symbols, levels,
                                      best_bids, best_asks, spreads)
            try:
                if self._clients:
                    self._clients[0].sendall(packet)
            except (ConnectionResetError, BrokenPipeError, OSError):
                pass
            self._last_send = now
            time.sleep(0.0005)

    def _advance_market(self):
        """Lightweight stochastic drift so the spread changes over time."""
        for book in self.books.values():
            drift = (0.5 - random.random()) * 0.05
            # Vary the spread: shift bids and asks in opposite directions
            spread_change = (random.random() - 0.5) * 2.0
            new_bids = {}
            new_asks = {}
            for price, size in book.bids.items():
                new_bids[price - spread_change] = max(0.1, size + drift * 2.0)
            for price, size in book.asks.items():
                new_asks[price + spread_change] = max(0.1, size + drift * 2.0)
            book.bids = new_bids
            book.asks = new_asks
            if random.random() < 0.001:
                side = "bid" if random.random() < 0.5 else "ask"
                base_price = book.best_bid() if side == "bid" else book.best_ask()
                if side == "bid":
                    book.bids[base_price - 0.5] = random.uniform(0.5, 5.0)
                else:
                    book.asks[base_price + 0.5] = random.uniform(0.5, 5.0)

    def start(self):
        self._running = True
        self._ensure_server()
        self._seed_books()
        import threading
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()
        # wait for C++ client to connect (single client on fd 0)
        while True:
            conn = self._connect_client()
            if conn is not None:
                print("[ABIDES] C++ application connected via UDS (fd 0).")
                break
            time.sleep(0.05)
        # send initial empty snapshot to connected client
        self._send(conn, build_packet(
            1, int(time.perf_counter_ns()), self.symbols,
            {s: [] for s in self.symbols}))
        self._last_send = time.perf_counter()

    def stop(self):
        self._running = False
        if self._server:
            self._server.close()
        if self._tcp_server:
            self._tcp_server.close()
        for conn in self._clients.values():
            conn.close()


def main():
    parser = argparse.ArgumentParser(description="ABIDES exchange simulator over UDS")
    parser.add_argument("--socket", default="/tmp/market_simulator.sock",
                        help="Unix domain socket path")
    parser.add_argument("--symbols", nargs="+", default=["BTCUSDT", "ETHUSDT"],
                        help="Symbols to simulate")
    parser.add_argument("--depth", type=int, default=20)
    parser.add_argument("--interval-us", type=float, default=4.0,
                        help="Snapshot interval in microseconds (default 4us = 250kHz)")
    parser.add_argument("--port", type=int, default=6789,
                        help="TCP port (used on Windows or with --tcp)")
    parser.add_argument("--tcp", action="store_true",
                        help="Use TCP loopback instead of Unix domain socket")
    args = parser.parse_args()

    if not args.tcp and os.path.exists(args.socket):
        os.remove(args.socket)

    sim = AbidesExchangeSimulator(
        socket_path=args.socket,
        symbols=args.symbols,
        depth=args.depth,
        snapshot_interval_ns=int(args.interval_us * 1000),
        port=args.port,
        use_tcp=args.tcp,
    )
    proto = "TCP" if args.tcp else "UDS"
    print(f"[ABIDES] Starting exchange simulator on {proto}")
    if args.tcp:
        print(f"[ABIDES] TCP port: {args.port}")
    else:
        print(f"[ABIDES] Socket: {args.socket}")
    print(f"[ABIDES] Symbols: {args.symbols}, depth: {args.depth}, "
          f"interval: {args.interval_us}us")
    sim.start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        sim.stop()


# (moved to end of file)



def _pack_i64(v: int) -> bytes:
    return struct.pack("<q", v)


def _pack_i64(v: int) -> bytes:
    return struct.pack("<q", v)


def build_packet(msg_type: int, timestamp_ns: int, symbols: List[str],
                 levels: Dict[str, List[tuple[float, float]]],
                 best_bids: Optional[Dict[str, float]] = None,
                 best_asks: Optional[Dict[str, float]] = None,
                 spreads: Optional[Dict[str, float]] = None) -> bytes:
    """Serialize orderbook snapshot/delta into a binary UDS packet.

    Packet layout per symbol:
      symbol_name : char[16] (null-padded)
      num_levels  : uint32
      best_bid    : int64  (price * 1e8)
      best_ask    : int64  (price * 1e8)
      spread      : int64  (price * 1e8)
      levels      : (price int64, size int64) x num_levels
    """
    best_bids = best_bids or {}
    best_asks = best_asks or {}
    spreads = spreads or {}

    payload = bytearray()
    for sym in symbols:
        name_bytes = sym.encode("ascii")[:16].ljust(16, b"\x00")
        payload += name_bytes
        sym_levels = levels.get(sym, [])
        payload += _pack_u32(len(sym_levels))
        bb = best_bids.get(sym, 0.0)
        ba = best_asks.get(sym, 0.0)
        sp = spreads.get(sym, ba - bb if ba and bb else 0.0)
        payload += _pack_i64(int(round(bb * 1e8)))
        payload += _pack_i64(int(round(ba * 1e8)))
        payload += _pack_i64(int(round(sp * 1e8)))
        for price, size in sym_levels:
            payload += _pack_i64(int(round(float(price) * 1e8)))
            payload += _pack_i64(int(round(float(size) * 1e8)))
    header = _pack_u32(0x4D4B5254)      # magic "MKRT"
    header += _pack_u32(len(payload))
    header += _pack_u32(msg_type)
    header += _pack_u64(timestamp_ns)
    return header + bytes(payload)


# ---------------------------------------------------------------------------
# Minimal in-process L2 order book
# ---------------------------------------------------------------------------

@dataclass
class OrderBook:
    symbol: str
    bids: Dict[float, float] = field(default_factory=dict)   # price -> size (desc)
    asks: Dict[float, float] = field(default_factory=dict)   # price -> size (asc)
    depth: int = 20
    scale: float = 1e8
    update_id: int = 0

    def best_bid(self) -> float:
        return max(self.bids) if self.bids else 0.0

    def best_ask(self) -> float:
        return min(self.asks) if self.asks else 0.0

    def spread(self) -> float:
        b, a = self.best_bid(), self.best_ask()
        return a - b if a and b else 0.0

    def add_trade(self, price: float, size: float, side: str) -> None:
        if side == "buy":
            self.bids[price] = self.bids.get(price, 0.0) + size
        else:
            self.asks[price] = self.asks.get(price, 0.0) + size


    def snapshot_dict(self) -> dict:
        return {
            "symbol": self.symbol,
            "bids": {str(p): s for p, s in sorted(self.bids.items(), reverse=True)[:self.depth]},
            "asks": {str(p): s for p, s in sorted(self.asks.items())[:self.depth]},
            "spread": self.spread(),
        }


if __name__ == "__main__":
    main()
