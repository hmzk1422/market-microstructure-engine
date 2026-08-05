"""
ArbitrageLimitOrderAgent.py
============================
Custom ABIDES agent that places passive limit orders at z-score entry
signals for the perp-spot arbitrage strategy.

When a z-score signal fires:
  - Direction +1 (short perp / long spot):
    -> Sell limit on perp ASK side (offer to sell at best ask)
    -> Buy limit on spot BID side (offer to buy at best bid)
  - Direction -1 (long perp / short spot):
    -> Buy limit on perp BID side (offer to buy at best bid)
    -> Sell limit on spot ASK side (offer to sell at best ask)

The agent monitors fills and cancels orders after a timeout.
Commission: 0.02% per leg = 0.04% round-trip (limit orders).
"""

import os
import pandas as pd
import numpy as np
from collections import defaultdict

from agent.TradingAgent import TradingAgent
from util.order.LimitOrder import LimitOrder
from util.util import log_print


# Conversion factor: 1 BTC = 100,000,000 satoshis
SATOSHI_PER_BTC = 100_000_000

# Price tick size for BTC pairs (0.1 USD). The arb agent places orders
# 1 tick BEHIND the best bid/ask so it doesn't interfere with the replay
# agent's L2 book reconstruction at the best price levels. This is also
# realistic passive limit order behavior: orders only fill if the market
# moves to our price.
TICK_SIZE = 0.1

# Limit order commission: 0.02% per leg = 0.04% round-trip
LIMIT_FEE_BPS = 0.0002  # per leg

# How long to wait for a limit order fill (in seconds)
MAX_LIMIT_WAIT_SEC = 5.0

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
                 entry_z=2.0, qty_grid_btc=QTY_GRID_BTC,
                 max_wait_sec=MAX_LIMIT_WAIT_SEC,
                 fee_bps=LIMIT_FEE_BPS, log_to_file=True):
        super().__init__(id, name, type, starting_cash=starting_cash,
                         log_orders=log_orders, random_state=random_state,
                         log_to_file=log_to_file)

        self.perp_symbol = perp_symbol
        self.spot_symbol = spot_symbol
        self.signals = signals
        self.entry_z = entry_z
        self.qty_grid_btc = qty_grid_btc
        self.max_wait_sec = max_wait_sec
        self.fee_bps = fee_bps

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
        direction = signal['direction']
        z_val = signal['z_val']
        signal_time = signal['time']

        perp_spread = self._current_spreads[self.perp_symbol]
        spot_spread = self._current_spreads[self.spot_symbol]

        for qty_btc in self.qty_grid_btc:
            qty_sat = int(round(qty_btc * SATOSHI_PER_BTC))

            # Place orders 1 tick BEHIND the best price to avoid interfering
            # with the replay agent's L2 book reconstruction at the best levels.
            # This is realistic passive limit order behavior: the order only
            # fills if the market moves to our price.
            if direction == 1:
                # Short perp: sell at ask + 1 tick (behind the ask)
                perp_price = perp_spread['ask'] + TICK_SIZE
                # Long spot: buy at bid - 1 tick (behind the bid)
                spot_price = spot_spread['bid'] - TICK_SIZE
                perp_is_buy = False
                spot_is_buy = True
            else:
                # Long perp: buy at bid - 1 tick (behind the bid)
                perp_price = perp_spread['bid'] - TICK_SIZE
                # Short spot: sell at ask + 1 tick (behind the ask)
                spot_price = spot_spread['ask'] + TICK_SIZE
                perp_is_buy = True
                spot_is_buy = False

            perp_oid = self._generate_oid()
            spot_oid = self._generate_oid()

            self.placeLimitOrder(
                symbol=self.perp_symbol, quantity=qty_sat,
                is_buy_order=perp_is_buy, limit_price=perp_price,
                order_id=perp_oid)
            self.placeLimitOrder(
                symbol=self.spot_symbol, quantity=qty_sat,
                is_buy_order=spot_is_buy, limit_price=spot_price,
                order_id=spot_oid)

            self.n_orders_placed += 2

            entry_key = f"{signal_time}_{qty_btc}_{direction}"
            entry = {
                'signal_time': signal_time, 'z_val': z_val,
                'direction': direction, 'qty_btc': qty_btc, 'qty_sat': qty_sat,
                'perp_oid': perp_oid, 'spot_oid': spot_oid,
                'perp_price': perp_price, 'spot_price': spot_price,
                'perp_is_buy': perp_is_buy, 'spot_is_buy': spot_is_buy,
                'perp_filled': False, 'spot_filled': False,
                'perp_fill_price': None, 'spot_fill_price': None,
                'perp_fill_time': None, 'spot_fill_time': None,
                'perp_fill_qty': 0.0, 'spot_fill_qty': 0.0,
                'perp_cancelled': False, 'spot_cancelled': False,
                'placed_at': currentTime, 'cancelled': False,
            }
            self._active_orders[entry_key] = entry
            # Build O(1) index for fill/cancel dispatch
            self._oid_index[perp_oid] = entry_key
            self._oid_index[spot_oid] = entry_key

    def _handle_fill(self, currentTime, order):
        # O(1) lookup via _oid_index instead of O(n) scan through _active_orders.
        # Also fixes the bug where fills on cancelled entries were silently dropped.
        entry_key = self._oid_index.get(order.order_id)
        if entry_key is None:
            return
        entry = self._active_orders.get(entry_key)
        if entry is None:
            return

        fill_qty = order.quantity / SATOSHI_PER_BTC  # convert satoshis to BTC

        if order.order_id == entry['perp_oid']:
            entry['perp_fill_qty'] += fill_qty
            entry['perp_fill_price'] = order.fill_price
            entry['perp_fill_time'] = currentTime
            self.n_fills += 1
            if entry['perp_fill_qty'] >= entry['qty_btc'] - 1e-9:
                entry['perp_filled'] = True
        elif order.order_id == entry['spot_oid']:
            entry['spot_fill_qty'] += fill_qty
            entry['spot_fill_price'] = order.fill_price
            entry['spot_fill_time'] = currentTime
            self.n_fills += 1
            if entry['spot_fill_qty'] >= entry['qty_btc'] - 1e-9:
                entry['spot_filled'] = True

        self._check_completed_entry(entry_key, entry, currentTime)

    def _handle_cancel(self, currentTime, order):
        # O(1) lookup via _oid_index instead of O(n) scan.
        entry_key = self._oid_index.get(order.order_id)
        if entry_key is None:
            return
        entry = self._active_orders.get(entry_key)
        if entry is None:
            return

        if order.order_id == entry['perp_oid']:
            entry['perp_cancelled'] = True
            self.n_cancels += 1
        elif order.order_id == entry['spot_oid']:
            entry['spot_cancelled'] = True
            self.n_cancels += 1

        self._check_completed_entry(entry_key, entry, currentTime)

    def _check_timeouts(self, currentTime):
        for entry_key, entry in list(self._active_orders.items()):
            if entry['cancelled'] or (entry['perp_filled'] and entry['spot_filled']):
                continue
            elapsed = (currentTime - entry['placed_at']).total_seconds()
            if elapsed > self.max_wait_sec:
                if not entry['perp_filled'] and entry['perp_oid'] in self.orders:
                    self.cancelOrder(self.orders[entry['perp_oid']])
                if not entry['spot_filled'] and entry['spot_oid'] in self.orders:
                    self.cancelOrder(self.orders[entry['spot_oid']])
                entry['cancelled'] = True
        self._check_completed_entries(currentTime)

    def _check_completed_entry(self, entry_key, entry, currentTime):
        """Check a single entry for completion. O(1) per call."""
        both_filled = entry['perp_filled'] and entry['spot_filled']
        both_done = (entry['perp_filled'] or entry['perp_cancelled']) and \
                    (entry['spot_filled'] or entry['spot_cancelled'])
        if both_filled or both_done:
            self._record_result(entry, currentTime)
            del self._active_orders[entry_key]
            # Clean up oid index entries
            self._oid_index.pop(entry['perp_oid'], None)
            self._oid_index.pop(entry['spot_oid'], None)

    def _check_completed_entries(self, currentTime):
        """Batch check all entries. Used by _check_timeouts."""
        completed = []
        for entry_key, entry in self._active_orders.items():
            both_filled = entry['perp_filled'] and entry['spot_filled']
            both_done = (entry['perp_filled'] or entry['perp_cancelled']) and \
                        (entry['spot_filled'] or entry['spot_cancelled'])
            if both_filled or both_done:
                self._record_result(entry, currentTime)
                completed.append(entry_key)
        for key in completed:
            entry = self._active_orders.pop(key, None)
            if entry:
                self._oid_index.pop(entry['perp_oid'], None)
                self._oid_index.pop(entry['spot_oid'], None)

    def _record_result(self, entry, currentTime):
        both_filled = entry['perp_filled'] and entry['spot_filled']
        direction = entry['direction']
        qty_btc = entry['qty_btc']

        if both_filled:
            if direction == 1:
                captured_spread = entry['perp_fill_price'] - entry['spot_fill_price']
            else:
                captured_spread = entry['spot_fill_price'] - entry['perp_fill_price']
            per_unit_fee = (entry['perp_fill_price'] * self.fee_bps) + \
                           (entry['spot_fill_price'] * self.fee_bps)
            total_fee = per_unit_fee * qty_btc
            net_spread = captured_spread - per_unit_fee
            if entry['perp_fill_time'] and entry['spot_fill_time']:
                ttf = max(
                    (entry['perp_fill_time'] - entry['signal_time']).total_seconds(),
                    (entry['spot_fill_time'] - entry['signal_time']).total_seconds())
            else:
                ttf = np.nan
        else:
            captured_spread = np.nan
            total_fee = 0.0
            net_spread = np.nan
            ttf = np.nan

        self.results.append({
            'signal_time': entry['signal_time'], 'entry_z': entry['z_val'],
            'direction': direction, 'qty': qty_btc,
            'method': 'limit_order_abides',
            'perp_limit_price': entry['perp_price'],
            'spot_limit_price': entry['spot_price'],
            'perp_filled': entry['perp_filled'],
            'spot_filled': entry['spot_filled'],
            'both_filled': both_filled,
            'perp_fill_price': entry['perp_fill_price'],
            'spot_fill_price': entry['spot_fill_price'],
            'perp_fill_time': entry['perp_fill_time'],
            'spot_fill_time': entry['spot_fill_time'],
            'perp_fill_qty': entry['perp_fill_qty'],
            'spot_fill_qty': entry['spot_fill_qty'],
            'time_to_fill_sec': ttf,
            'cancelled': entry['cancelled'],
            'captured_spread': captured_spread,
            'fee_cost': total_fee, 'fee_bps': self.fee_bps * 10000,
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
        print(f"\n--- ABIDES Limit Order Summary ---")
        print(f"Total signals attempted: {n_total}")
        if n_total > 0:
            print(f"Both legs filled: {n_filled} ({n_filled/n_total*100:.1f}%)")
            print(f"Cancelled: {n_cancelled} ({n_cancelled/n_total*100:.1f}%)")

        if n_filled > 0:
            df_filled = df[df['both_filled']]
            print(f"\n--- Fill Rate by Order Size ---")
            for qty in sorted(df['qty'].unique()):
                sub = df_filled[df_filled['qty'] == qty]
                total = len(df[df['qty'] == qty])
                print(f"  {qty:>8.2f} BTC: {len(sub)}/{total} filled ({len(sub)/total*100:.1f}%)")

            print(f"\n--- Captured Spread (filled only) ---")
            for qty in sorted(df_filled['qty'].unique()):
                sub = df_filled[df_filled['qty'] == qty]
                print(f"  {qty:>8.2f} BTC: mean={sub['net_spread'].mean():.2f}, "
                      f"median={sub['net_spread'].median():.2f}")