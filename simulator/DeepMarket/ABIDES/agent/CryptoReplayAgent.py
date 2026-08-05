"""
CryptoReplayAgent.py
=====================
Custom ABIDES agent that replays historical L2 order book data from
TimescaleDB (Binance crypto data) into the ABIDES ExchangeAgent.

PERFORMANCE DESIGN
------------------
This agent INJECTS order book state DIRECTLY into the exchange's OrderBook
by calling handleLimitOrder/cancelOrder synchronously, BYPASSING the kernel
message queue. This is possible because the replay agent IS the market —
it controls all the counterparty liquidity.

Routing every L2 event through the kernel message queue was the cause of
the quadratic slowdown: 40K events generated ~100K kernel messages, each
with PriorityQueue and Message object overhead. Direct injection reduces
this to O(events) synchronous calls.

Fills of OTHER agents' orders (e.g. the arbitrage agent's resting orders)
still work correctly: when the replay's injected order crosses the spread,
the OrderBook's handleLimitOrder detects the match and sends ORDER_EXECUTED
to the counterparty via the kernel. So the arb agent's fill detection is
preserved.

Prices are kept as floats (ABIDES OrderBook doesn't enforce integer prices).
Quantities are converted to satoshis (1 BTC = 100,000,000 satoshis) to
satisfy the OrderBook's integer quantity requirement.
"""

import pandas as pd
import numpy as np
from sqlalchemy import create_engine
from bisect import bisect_right

from agent.TradingAgent import TradingAgent
from util.order.LimitOrder import LimitOrder
from util.util import log_print


# Conversion factor: 1 BTC = 100,000,000 satoshis
SATOSHI_PER_BTC = 100_000_000

DB_URL = "postgresql://market:market@localhost:5433/market_data"


class CryptoReplayAgent(TradingAgent):
    """
    Replays historical L2 order book data from TimescaleDB into the
    ABIDES ExchangeAgent, using DIRECT OrderBook injection for speed.
    Maintains AT MOST ONE order per price level.
    """

    def __init__(self, id, name, type, symbol, start_time, end_time,
                 db_url=DB_URL, starting_cash=0, log_orders=False,
                 random_state=None, limit=1_000_000, log_to_file=True):
        super().__init__(id, name, type, starting_cash=starting_cash,
                         log_orders=log_orders, random_state=random_state,
                         log_to_file=log_to_file)

        self.symbol = symbol
        self.start_time = start_time
        self.end_time = end_time
        self.db_url = db_url
        self.limit = limit

        # Track one order per (side, price) level.
        # Key: (side, price) -> (order_id, quantity_satoshis)
        self._order_tracker = {}

        # Counter for generating unique order IDs
        self._order_counter = 0

        # Pre-loaded events and wakeup schedule
        self.events = None
        self.wakeup_times = None
        self._events_by_time = None

        # Stats
        self.orders_placed = 0
        self.orders_cancelled = 0
        self.snapshots_processed = 0

    def _generate_order_id(self):
        """Generate a unique order ID for this agent."""
        oid = f"{self.id}_{self._order_counter}"
        self._order_counter += 1
        return oid

    def _load_events(self):
        """Load LOB events from TimescaleDB."""
        engine = create_engine(self.db_url)

        query = f"""
            SELECT time, side, price, size, is_snapshot, seq
            FROM orderbook
            WHERE symbol = '{self.symbol}'
            ORDER BY time ASC, seq ASC
            LIMIT {self.limit};
        """

        print(f"[{self.symbol}] Loading LOB events from database...")
        df = pd.read_sql(query, engine)

        if df.empty:
            print(f"[{self.symbol}] No events found!")
            return

        # Strip timezone for consistency
        if hasattr(df['time'], 'dt') and df['time'].dt.tz is not None:
            df['time'] = df['time'].dt.tz_localize(None)

        # Filter to simulation window
        df = df[(df['time'] >= self.start_time) & (df['time'] <= self.end_time)]

        print(f"[{self.symbol}] Loaded {len(df)} events in simulation window.")

        # Group events by timestamp
        df = df.sort_values(['time', 'seq']).reset_index(drop=True)
        self._events_by_time = {ts: group for ts, group in df.groupby('time')}
        self.wakeup_times = sorted(self._events_by_time.keys())

        if self.wakeup_times:
            print(f"[{self.symbol}] First event: {self.wakeup_times[0]}")
            print(f"[{self.symbol}] Last event: {self.wakeup_times[-1]}")
            print(f"[{self.symbol}] Unique timestamps: {len(self.wakeup_times)}")

    def wakeup(self, currentTime):
        """Called by the Kernel. Inject all L2 events at this timestamp DIRECTLY
        into the exchange order book (no kernel message traffic for replay)."""
        super().wakeup(currentTime)

        if not self.mkt_open or not self.mkt_close:
            return

        # O(log n) lookup of next wakeup time using bisect
        idx = bisect_right(self.wakeup_times, currentTime)
        if idx < len(self.wakeup_times):
            self.setWakeup(self.wakeup_times[idx])

        # Process all events at this timestamp
        if currentTime in self._events_by_time:
            events = self._events_by_time[currentTime]
            for event in events.itertuples(index=False):
                self._process_event(currentTime, event)

    def _get_actual_size(self, side, price):
        """Query the ACTUAL size at a price level from the exchange order book.
        This accounts for orders placed by other agents (e.g. arb agent) at the
        same price, which the replay agent's own tracker does not know about."""
        try:
            book = self.exchange.order_books[self.symbol]
            return book.getSizeAtPrice(side, price)
        except (AttributeError, KeyError):
            return self._get_existing_size(side, price)

    def _get_existing_size(self, side, price):
        """Get the tracked size of OUR order at a price level (0 if none)."""
        key = (side, price)
        if key not in self._order_tracker:
            return 0
        return self._order_tracker[key][1]

    def _process_event(self, currentTime, event):
        """Process a single LOB event. Maintains ONE order per price level."""
        side = event.side  # 'bid' or 'ask'
        price = float(event.price)
        size_btc = float(event.size)
        is_snapshot = bool(event.is_snapshot)

        # Convert size to satoshis
        size_sat = int(round(size_btc * SATOSHI_PER_BTC))

        is_buy = (side == 'bid')

        if is_snapshot:
            # Full snapshot: clear ALL our orders, then set the book from scratch.
            self._clear_all()
            if size_sat > 0:
                self._set_order(currentTime, side, price, size_sat, is_buy)
            self.snapshots_processed += 1
        else:
            if size_sat == 0:
                # Price level removed: cancel our order at this price if any.
                self._set_order(currentTime, side, price, 0, is_buy)
            else:
                # The L2 event tells us the TOTAL size at this price should be size_sat.
                # Determine OUR portion = size_sat minus the portion owned by
                # other agents (arb agent) currently in the book at this price.
                actual_size = self._get_actual_size(side, price)
                our_size = self._get_existing_size(side, price)

                other_size = actual_size - our_size
                if other_size < 0:
                    other_size = 0  # safety: our tracker may be ahead of reality

                our_target = size_sat - other_size
                if our_target < 0:
                    our_target = 0

                # Set our order to exactly our_target (0 = cancel/remove).
                self._set_order(currentTime, side, price, our_target, is_buy)

    def _set_order(self, currentTime, side, price, target_size_sat, is_buy):
        """Set OUR order at a price level to exactly target_size_sat.
        Uses OrderBook.setLevel which mutates the book DIRECTLY without
        generating ORDER_ACCEPTED/ORDER_CANCELLED messages for the replay
        agent. Crosses against OTHER agents' resting orders still generate
        ORDER_EXECUTED messages to those agents (preserving fill detection)."""
        key = (side, price)
        book = self.exchange.order_books[self.symbol]

        # Use a stable order_id for this price level so setLevel can update
        # the existing order in place when the size changes.
        if key in self._order_tracker:
            oid, _ = self._order_tracker[key]
        else:
            oid = self._generate_order_id()

        # Directly set the level (no kernel messages for our own events).
        # setLevel handles crossing against other agents' resting orders.
        book.setLevel(side, price, target_size_sat, oid)

        if target_size_sat == 0:
            if key in self._order_tracker:
                del self._order_tracker[key]
            self.orders_cancelled += 1
        else:
            self._order_tracker[key] = (oid, target_size_sat)
            self.orders_placed += 1

    def _clear_all(self):
        """Cancel ALL our outstanding orders (on both sides) directly in the book."""
        book = self.exchange.order_books[self.symbol]
        for key in list(self._order_tracker.keys()):
            side, price = key
            oid, qty = self._order_tracker[key]
            level_list = book.bids if side == 'bid' else book.asks
            for li, orders in enumerate(level_list):
                if orders and orders[0].limit_price == price:
                    for oi, o in enumerate(orders):
                        if o.order_id == oid:
                            orders.pop(oi)
                            if not orders:
                                level_list.pop(li)
                            break
                    break
            del self._order_tracker[key]
            self.orders_cancelled += 1

    def kernelStarting(self, startTime):
        """Find the exchange agent so we can query actual book state."""
        super().kernelStarting(startTime)
        from agent.ExchangeAgent import ExchangeAgent
        self.exchangeID = self.kernel.findAgentByType(ExchangeAgent)
        self.exchange = self.kernel.agents[self.exchangeID]

    def getWakeFrequency(self):
        """Return the wake frequency for the first wakeup."""
        if self.wakeup_times is None:
            self._load_events()

        if self.wakeup_times:
            return self.wakeup_times[0] - self.mkt_open
        return pd.Timedelta(seconds=1)

    def kernelStopping(self):
        """Log stats when the kernel stops."""
        super().kernelStopping()
        print(f"\n[{self.symbol}] CryptoReplayAgent Stats:")
        print(f"  Orders placed: {self.orders_placed}")
        print(f"  Orders cancelled: {self.orders_cancelled}")
        print(f"  Snapshots processed: {self.snapshots_processed}")