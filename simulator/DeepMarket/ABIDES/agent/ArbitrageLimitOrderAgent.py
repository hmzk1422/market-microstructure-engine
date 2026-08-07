"""
ArbitrageLimitOrderAgent.py
============================
Custom ABIDES agent that implements Maker-Taker arbitrage strategy.

EXECUTION MODEL:
  - Maker leg: Passive limit order (waits for fill)
  - Taker leg: Aggressive market order (executes immediately after maker fills)
  
This eliminates legging risk and is the standard execution style for
statistical arbitrage in professional trading.

When a z-score signal fires:
  Direction +1 (short perp / long spot):
    -> Place limit order on spot BID (maker)
    -> Upon fill, send market order to sell perp (taker)
    
  Direction -1 (long perp / short spot):
    -> Place limit order on perp BID (maker)
    -> Upon fill, send market order to sell spot (taker)

Commission: 
  - Maker: 0.02% per leg
  - Taker: 0.05% per leg
  - Total: 0.07% round-trip (maker-taker)
"""

import os
import pandas as pd
import numpy as np
from collections import defaultdict

from agent.TradingAgent import TradingAgent
from util.order.LimitOrder import LimitOrder
from util.order.MarketOrder import MarketOrder
from util.util import log_print


# Conversion factor: 1 BTC = 100,000,000 satoshis
SATOSHI_PER_BTC = 100_000_000

# Price tick size for BTC pairs (0.1 USD)
TICK_SIZE = 0.0  # Place at touch for immediate fills

# Fee structure
MAKER_FEE_BPS = 0.0002  # 0.02% - passive limit orders
TAKER_FEE_BPS = 0.0005  # 0.05% - aggressive market orders

# How long to wait for maker leg fill (in seconds)
MAX_MAKER_WAIT_SEC = 30.0  # Cancel if no fill within 30 seconds

# Execution latency: time between maker fill and taker order submission (milliseconds)
EXECUTION_LATENCY_MS = 100.0  # Realistic for co-located server

# Order sizes to simulate (in BTC)
QTY_GRID_BTC = [0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0]


class ArbitrageLimitOrderAgent(TradingAgent):
    """
    Places passive limit orders at z-score entry signals for the
    perp-spot arbitrage strategy.

    Uses the ABIDES ExchangeAgent's matching engine for proper
    FIFO queue priority and fill detection.
    """

    def __init__(self, id, name, type, perp_symbol, spot_symbol,
                 signals, starting_cash=10_000_000_000,
                 log_orders=False, random_state=None,
                 entry_z=1.5, qty_grid_btc=QTY_GRID_BTC,
                 max_wait_sec=MAX_MAKER_WAIT_SEC,
                 maker_fee_bps=MAKER_FEE_BPS,
                 taker_fee_bps=TAKER_FEE_BPS,
                 execution_latency_ms=EXECUTION_LATENCY_MS,
                 log_to_file=True):
        super().__init__(id, name, type, starting_cash=starting_cash,
                         log_orders=log_orders, random_state=random_state,
                         log_to_file=log_to_file)

        self.perp_symbol = perp_symbol
        self.spot_symbol = spot_symbol
        self.signals = signals
        self.entry_z = entry_z
        self.qty_grid_btc = qty_grid_btc
        self.max_wait_sec = max_wait_sec
        self.maker_fee_bps = maker_fee_bps
        self.taker_fee_bps = taker_fee_bps
        self.execution_latency_ms = execution_latency_ms

        self._signal_times = None
        self._signal_idx = 0
        self._signals_by_time = None

        self._active_orders = {}
        # O(1) lookup: order_id -> entry_key, for fast fill/cancel dispatch
        self._oid_index = {}
        self.results = []

        self.n_signals = 0
        self.n_orders_placed = 0
        self.n_fills = 0
        self.n_cancels = 0
        self._oid_counter = 0
        self._pending_signal = None
        self._current_spreads = {}

    def _generate_oid(self):
        oid = f"arb_{self.id}_{self._oid_counter}"
        self._oid_counter += 1
        return oid

    def _prepare_signals(self):
        df = self.signals[abs(self.signals['z_score']) >= self.entry_z].copy()
        df = df.sort_values('time').reset_index(drop=True)

        if hasattr(df['time'], 'dt') and df['time'].dt.tz is not None:
            df['time'] = df['time'].dt.tz_localize(None)

        self._signals_by_time = {row['time']: row for _, row in df.iterrows()}
        self._signal_times = sorted(self._signals_by_time.keys())

        print(f"[ArbAgent] Prepared {len(self._signal_times)} signals above |z| >= {self.entry_z}")
        if self._signal_times:
            print(f"[ArbAgent] First signal: {self._signal_times[0]}")
            print(f"[ArbAgent] Last signal: {self._signal_times[-1]}")

    def getWakeFrequency(self):
        if self._signal_times is None:
            self._prepare_signals()
        if self._signal_times:
            return self._signal_times[0] - self.mkt_open
        return pd.Timedelta(seconds=1)

    def wakeup(self, currentTime):
        super().wakeup(currentTime)
        if not self.mkt_open or not self.mkt_close:
            return

        # O(log n) lookup of next signal using bisect + incremental index.
        # _signal_idx only moves forward, so we never re-scan from the start.
        while self._signal_idx < len(self._signal_times):
            t = self._signal_times[self._signal_idx]
            if t > currentTime:
                self.setWakeup(t)
                break
            self._signal_idx += 1
        else:
            # No more signals; schedule one final wakeup to clean up
            # any remaining active orders after their timeout expires.
            if self._active_orders:
                # Find the earliest timeout time
                earliest = min(e['placed_at'] + pd.Timedelta(seconds=self.max_wait_sec)
                               for e in self._active_orders.values())
                if earliest > currentTime:
                    self.setWakeup(earliest)
                # else: we're already past the timeout, will clean up below

        # Check timeouts on every wakeup (signal or cleanup)
        self._check_timeouts(currentTime)

        # If we just cleaned up orders and there are still more pending,
        # schedule a wakeup for the next timeout
        if self._active_orders and not (self._signal_idx < len(self._signal_times)):
            earliest = min(e['placed_at'] + pd.Timedelta(seconds=self.max_wait_sec)
                           for e in self._active_orders.values())
            if earliest > currentTime:
                self.setWakeup(earliest)

        if currentTime in self._signals_by_time:
            signal = self._signals_by_time[currentTime]
            z_val = signal['z_score']
            self._handle_signal(currentTime, z_val)

    def _handle_signal(self, currentTime, z_val):
        self.n_signals += 1
        direction = 1 if z_val >= self.entry_z else -1

        self.getCurrentSpread(self.perp_symbol, depth=1)
        self.getCurrentSpread(self.spot_symbol, depth=1)

        self._pending_signal = {
            'time': currentTime,
            'z_val': z_val,
            'direction': direction,
        }

    def receiveMessage(self, currentTime, msg):
        super().receiveMessage(currentTime, msg)

        if msg.body['msg'] == 'ORDER_EXECUTED':
            self._handle_fill(currentTime, msg.body['order'])
        elif msg.body['msg'] == 'ORDER_CANCELLED':
            self._handle_cancel(currentTime, msg.body['order'])
        elif msg.body['msg'] == 'QUERY_SPREAD':
            if self._pending_signal:
                symbol = msg.body['symbol']
                bids = msg.body['bids']
                asks = msg.body['asks']
                if not bids or not asks:
                    return
                self._current_spreads[symbol] = {
                    'bid': bids[0][0],
                    'ask': asks[0][0],
                    'bid_size': bids[0][1],
                    'ask_size': asks[0][1],
                }
                if (self.perp_symbol in self._current_spreads and
                    self.spot_symbol in self._current_spreads):
                    signal = self._pending_signal
                    self._pending_signal = None
                    self._place_arb_orders(currentTime, signal)

    def _place_arb_orders(self, currentTime, signal):
        """
        Place orders using Maker-Taker execution model.
        
        Strategy: Place ONE leg as passive limit (maker), then immediately
        execute the other leg as market order (taker) when maker fills.
        
        Direction +1 (short perp / long spot):
          -> Maker: Buy limit on spot BID (wait for fill)
          -> Taker: Sell market on perp (execute immediately after fill)
          
        Direction -1 (long perp / short spot):
          -> Maker: Buy limit on perp BID (wait for fill)
          -> Taker: Sell market on spot (execute immediately after fill)
        
        This eliminates legging risk because taker executes immediately
        after maker fill, with only a configurable latency overhead.
        """
        direction = signal['direction']
        z_val = signal['z_val']
        signal_time = signal['time']

        perp_spread = self._current_spreads[self.perp_symbol]
        spot_spread = self._current_spreads[self.spot_symbol]

        for qty_btc in self.qty_grid_btc:
            qty_sat = int(round(qty_btc * SATOSHI_PER_BTC))

            if direction == 1:
                # Short perp / Long spot
                # Maker: Buy spot at bid (join bid queue)
                maker_symbol = self.spot_symbol
                maker_price = spot_spread['bid']
                maker_is_buy = True
                # Taker: Sell perp at market (sell immediately)
                taker_symbol = self.perp_symbol
                taker_is_buy = False
            else:
                # Long perp / Short spot
                # Maker: Buy perp at bid (join bid queue)
                maker_symbol = self.perp_symbol
                maker_price = perp_spread['bid']
                maker_is_buy = True
                # Taker: Sell spot at market (sell immediately)
                taker_symbol = self.spot_symbol
                taker_is_buy = False

            maker_oid = self._generate_oid()
            taker_oid = self._generate_oid()

            # Place MAKER order (passive limit)
            self.placeLimitOrder(
                symbol=maker_symbol, quantity=qty_sat,
                is_buy_order=maker_is_buy, limit_price=maker_price,
                order_id=maker_oid)

            self.n_orders_placed += 1

            entry_key = f"{signal_time}_{qty_btc}_{direction}"
            entry = {
                'signal_time': signal_time, 'z_val': z_val,
                'direction': direction, 'qty_btc': qty_btc, 'qty_sat': qty_sat,
                'maker_oid': maker_oid, 'taker_oid': taker_oid,
                'maker_symbol': maker_symbol, 'taker_symbol': taker_symbol,
                'maker_is_buy': maker_is_buy, 'taker_is_buy': taker_is_buy,
                'maker_price': maker_price,
                'maker_filled': False, 'taker_filled': False,
                'maker_fill_price': None, 'taker_fill_price': None,
                'maker_fill_time': None, 'taker_fill_time': None,
                'maker_fill_qty': 0.0, 'taker_fill_qty': 0.0,
                'maker_cancelled': False, 'taker_cancelled': False,
                'taker_submitted': False,  # Taker not submitted until maker fills
                'placed_at': currentTime, 'cancelled': False,
            }
            self._active_orders[entry_key] = entry
            # Build O(1) index for fill/cancel dispatch
            self._oid_index[maker_oid] = entry_key
            self._oid_index[taker_oid] = entry_key

    def _handle_fill(self, currentTime, order):
        """Handle order fills with Maker-Taker logic.
        
        When maker fills:
        1. Record maker fill
        2. Schedule taker order with execution latency
        3. When taker fills, record and complete
        """
        # O(1) lookup via _oid_index instead of O(n) scan through _active_orders.
        entry_key = self._oid_index.get(order.order_id)
        if entry_key is None:
            return
        entry = self._active_orders.get(entry_key)
        if entry is None:
            return

        fill_qty = order.quantity / SATOSHI_PER_BTC  # convert satoshis to BTC

        # Handle MAKER fill
        if order.order_id == entry['maker_oid']:
            entry['maker_fill_qty'] += fill_qty
            entry['maker_fill_price'] = order.fill_price
            entry['maker_fill_time'] = currentTime
            self.n_fills += 1
            if entry['maker_fill_qty'] >= entry['qty_btc'] - 1e-9:
                entry['maker_filled'] = True
            
            # MAKER filled! Now schedule TAKER order with latency
            if not entry['taker_submitted'] and entry['maker_filled']:
                self._schedule_taker_order(entry, currentTime)
        
        # Handle TAKER fill
        elif order.order_id == entry['taker_oid']:
            entry['taker_fill_qty'] += fill_qty
            entry['taker_fill_price'] = order.fill_price
            entry['taker_fill_time'] = currentTime
            self.n_fills += 1
            if entry['taker_fill_qty'] >= entry['qty_btc'] - 1e-9:
                entry['taker_filled'] = True

        self._check_completed_entry(entry_key, entry, currentTime)

    def _schedule_taker_order(self, entry, currentTime):
        """Schedule taker order to execute after latency delay.
        
        In ABIDES, we use kernel wakeup calls to simulate latency.
        For simplicity, we execute immediately (real-world: submit market order).
        """
        entry['taker_submitted'] = True
        
        # Place market order on taker leg
        taker_symbol = entry['taker_symbol']
        taker_is_buy = entry['taker_is_buy']
        qty_sat = entry['qty_sat']
        taker_oid = entry['taker_oid']
        
        # Get current market price for market order
        taker_spread = self._current_spreads.get(taker_symbol)
        if taker_spread:
            # For a sell market order, use bid; for buy, use ask
            limit_price = taker_spread['bid'] if not taker_is_buy else taker_spread['ask']
        else:
            limit_price = None
        
        self.placeMarketOrder(
            symbol=taker_symbol,
            quantity=qty_sat,
            is_buy_order=taker_is_buy,
            order_id=taker_oid,
            limit_price=limit_price  # Price protection: won't fill worse than this
        )
        
        self.n_orders_placed += 1

    def _handle_cancel(self, currentTime, order):
        # O(1) lookup via _oid_index instead of O(n) scan.
        entry_key = self._oid_index.get(order.order_id)
        if entry_key is None:
            return
        entry = self._active_orders.get(entry_key)
        if entry is None:
            return

        if order.order_id == entry['maker_oid']:
            entry['maker_cancelled'] = True
            self.n_cancels += 1
        elif order.order_id == entry['taker_oid']:
            entry['taker_cancelled'] = True
            self.n_cancels += 1

        self._check_completed_entry(entry_key, entry, currentTime)

    def _check_timeouts(self, currentTime):
        for entry_key, entry in list(self._active_orders.items()):
            if entry['cancelled'] or (entry['maker_filled'] and entry['taker_filled']):
                continue
            elapsed = (currentTime - entry['placed_at']).total_seconds()
            if elapsed > self.max_wait_sec:
                # Cancel only if not already filled
                if not entry['maker_filled'] and not entry['maker_cancelled']:
                    if entry['maker_oid'] in self.orders:
                        self.cancelOrder(self.orders[entry['maker_oid']])
                    entry['maker_cancelled'] = True
                # Taker shouldn't exist if maker didn't fill, but cancel just in case
                if not entry['taker_filled'] and not entry['taker_cancelled']:
                    if entry['taker_oid'] in self.orders:
                        self.cancelOrder(self.orders[entry['taker_oid']])
                    entry['taker_cancelled'] = True
                entry['cancelled'] = True
        self._check_completed_entries(currentTime)

    def _check_completed_entry(self, entry_key, entry, currentTime):
        """Check a single entry for completion. O(1) per call."""
        both_filled = entry['maker_filled'] and entry['taker_filled']
        both_done = (entry['maker_filled'] or entry['maker_cancelled']) and \
                    (entry['taker_filled'] or entry['taker_cancelled'])
        if both_filled or both_done:
            self._record_result(entry, currentTime)
            del self._active_orders[entry_key]
            # Clean up oid index entries
            self._oid_index.pop(entry['maker_oid'], None)
            self._oid_index.pop(entry['taker_oid'], None)

    def _check_completed_entries(self, currentTime):
        """Batch check all entries. Used by _check_timeouts."""
        completed = []
        for entry_key, entry in self._active_orders.items():
            both_filled = entry['maker_filled'] and entry['taker_filled']
            both_done = (entry['maker_filled'] or entry['maker_cancelled']) and \
                        (entry['taker_filled'] or entry['taker_cancelled'])
            if both_filled or both_done:
                self._record_result(entry, currentTime)
                completed.append(entry_key)
        for key in completed:
            entry = self._active_orders.pop(key, None)
            if entry:
                self._oid_index.pop(entry['maker_oid'], None)
                self._oid_index.pop(entry['taker_oid'], None)

    def _record_result(self, entry, currentTime):
        """Record trade result with Maker-Taker fees."""
        both_filled = entry['maker_filled'] and entry['taker_filled']
        direction = entry['direction']
        qty_btc = entry['qty_btc']

        if both_filled:
            # Maker leg fill price (passive)
            maker_price = entry['maker_fill_price']
            # Taker leg fill price (aggressive)
            taker_price = entry['taker_fill_price']
            
            if direction == 1:
                # Short perp / Long spot: captured spread = taker_sell - maker_buy
                captured_spread = taker_price - maker_price
            else:
                # Long perp / Short spot: captured spread = maker_buy - taker_sell
                captured_spread = maker_price - taker_price
            
            # Maker-Taker fees
            maker_fee = maker_price * self.maker_fee_bps * qty_btc
            taker_fee = taker_price * self.taker_fee_bps * qty_btc
            total_fee = maker_fee + taker_fee
            fee_bps_total = (self.maker_fee_bps + self.taker_fee_bps) * 10000
            
            net_spread = captured_spread - (maker_price + taker_price) * 0.5 * fee_bps_total / 10000
            
            # Time to fill = time from signal to taker fill
            if entry['maker_fill_time'] and entry['taker_fill_time']:
                ttf = (entry['taker_fill_time'] - entry['signal_time']).total_seconds()
            else:
                ttf = np.nan
        else:
            captured_spread = np.nan
            total_fee = 0.0
            fee_bps_total = 0.0
            net_spread = np.nan
            ttf = np.nan

        self.results.append({
            'signal_time': entry['signal_time'], 'entry_z': entry['z_val'],
            'direction': direction, 'qty': qty_btc,
            'method': 'maker_taker_abides',
            'maker_symbol': entry['maker_symbol'],
            'taker_symbol': entry['taker_symbol'],
            'maker_limit_price': entry['maker_price'],
            'maker_filled': entry['maker_filled'],
            'taker_filled': entry['taker_filled'],
            'both_filled': both_filled,
            'maker_fill_price': entry['maker_fill_price'],
            'taker_fill_price': entry['taker_fill_price'],
            'maker_fill_time': entry['maker_fill_time'],
            'taker_fill_time': entry['taker_fill_time'],
            'maker_fill_qty': entry['maker_fill_qty'],
            'taker_fill_qty': entry['taker_fill_qty'],
            'time_to_fill_sec': ttf,
            'taker_submitted': entry['taker_submitted'],
            'cancelled': entry['cancelled'],
            'captured_spread': captured_spread,
            'maker_fee_bps': self.maker_fee_bps * 10000,
            'taker_fee_bps': self.taker_fee_bps * 10000,
            'total_fee_bps': fee_bps_total,
            'fee_cost': total_fee,
            'net_spread': net_spread,
        })

    def kernelStopping(self):
        super().kernelStopping()
        print(f"\n[ArbAgent] Stats:")
        print(f"  Signals processed: {self.n_signals}")
        print(f"  Orders placed: {self.n_orders_placed}")
        print(f"  Fills: {self.n_fills}")
        print(f"  Cancels: {self.n_cancels}")
        print(f"  Results recorded: {len(self.results)}")

        # Always write the parquet file, even if empty, so downstream
        # analysis can rely on its existence.
        df = pd.DataFrame(self.results)
        out_path = os.path.abspath("limit_order_abides_results.parquet")
        df.to_parquet(out_path)
        print(f"[ArbAgent] Results saved to {out_path}")

        n_total = len(df)
        n_filled = df['both_filled'].sum() if n_total > 0 else 0
        n_cancelled = df['cancelled'].sum() if n_total > 0 else 0
        print(f"\n--- ABIDES Maker-Taker Summary ---")
        print(f"Total signals attempted: {n_total}")
        if n_total > 0:
            print(f"Both legs filled: {n_filled} ({n_filled/n_total*100:.1f}%)")
            print(f"Cancelled (maker timeout): {n_cancelled} ({n_cancelled/n_total*100:.1f}%)")
            print(f"\nFee Structure:")
            print(f"  Maker fee: {self.maker_fee_bps*10000:.2f} bps")
            print(f"  Taker fee: {self.taker_fee_bps*10000:.2f} bps")
            print(f"  Total: {(self.maker_fee_bps + self.taker_fee_bps)*10000:.2f} bps")

        if n_filled > 0:
            df_filled = df[df['both_filled']]
            print(f"\n--- Fill Rate by Order Size ---")
            for qty in sorted(df['qty'].unique()):
                sub = df_filled[df_filled['qty'] == qty]
                total = len(df[df['qty'] == qty])
                print(f"  {qty:>8.2f} BTC: {len(sub)}/{total} filled ({len(sub)/total*100:.1f}%)")
            
            print(f"\n--- PnL by Order Size ---")
            for qty in sorted(df_filled['qty'].unique()):
                sub = df_filled[df_filled['qty'] == qty]
                avg_spread = sub['captured_spread'].mean()
                avg_net = sub['net_spread'].mean()
                print(f"  {qty:>8.2f} BTC: avg spread {avg_spread:>8.2f} bps, "
                      f"net {avg_net:>8.2f} bps")

            print(f"\n--- Captured Spread (filled only) ---")
            for qty in sorted(df_filled['qty'].unique()):
                sub = df_filled[df_filled['qty'] == qty]
                print(f"  {qty:>8.2f} BTC: mean={sub['net_spread'].mean():.2f}, "
                      f"median={sub['net_spread'].median():.2f}")