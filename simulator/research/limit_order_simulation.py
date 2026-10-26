"""
limit_order_simulation.py
==========================
Phase 3: Limit Order Simulation with DeepMarket-inspired Execution

Simulates placing passive limit orders (instead of aggressive market orders)
at z-score entry signals, using the live order book state to determine:

  1. Queue position (where we are in line at the best bid/ask)
  2. Fill probability (will our order get filled before the signal decays?)
  3. Time-to-fill (how long until we're executed)
  4. Partial fills (if the queue only partially moves to our level)
  5. Adverse selection (did we get filled just before a move against us?)

Commission structure (limit orders): 0.02% per leg → 0.04% round-trip
Commission structure (market orders): 0.05% per leg → 0.10% round-trip

This is then compared against the aggressive market order approach from
Phase 1 (static) and Phase 2 (dynamic) to determine which execution
strategy is more profitable for our arbitrage signals.
"""

import numpy as np
import pandas as pd
from sqlalchemy import create_engine
from simulator.research.preprocessing import prepare_signals
from simulator.research.liquidity_capacity import (
    reconstruct_book_with_depth,
    FEE_HURDLE_BPS,
    QTY_GRID,
    N_LEVELS,
    generate_capacity_report,
    walk_book_vwap,
)

DB_URL = "postgresql://market:market@localhost:5433/market_data"

# Limit order commission: 0.02% per leg = 0.04% round-trip
LIMIT_FEE_HURDLE_BPS = 0.0002  # per leg
LIMIT_FEE_HURDLE_TOTAL = 0.0004  # round-trip

# Market order commission: 0.05% per leg = 0.10% round-trip (from Phase 1)
MARKET_FEE_HURDLE_BPS = 0.0005  # per leg
MARKET_FEE_HURDLE_TOTAL = 0.0010  # round-trip

# How long we're willing to wait for a limit order fill (in nanoseconds)
# After this, we cancel and either give up or switch to market order
MAX_LIMIT_WAIT_NS = 5_000_000_000  # 5 seconds

# Minimum queue position to be "first in line"
# If our order would be behind more than this many contracts, we skip
MAX_QUEUE_DEPTH_BTC = 5.0  # BTC


# =====================================================================
# 1. LIVE ORDER BOOK STATE MACHINE (extended from Phase 2)
# =====================================================================
class LiveOrderBook:
    """Maintains a live top-N order book state from raw LOB events."""

    def __init__(self, symbol, levels=N_LEVELS):
        self.symbol = symbol
        self.levels = levels
        self.bids = {}  # price -> size
        self.asks = {}  # price -> size
        self.current_time = None
        self.current_seq = None

    def apply_event(self, time, side, price, size, is_snapshot, seq):
        """Apply a single LOB event to the book state."""
        self.current_time = time
        self.current_seq = seq

        if is_snapshot:
            if side == 'bid':
                self.bids.clear()
            else:
                self.asks.clear()

        book_side = self.bids if side == 'bid' else self.asks
        if size == 0:
            book_side.pop(price, None)
        else:
            book_side[price] = size

    def snapshot(self):
        """Return the current top-N levels as a dict."""
        bid_prices = sorted(self.bids.keys(), reverse=True)[:self.levels]
        ask_prices = sorted(self.asks.keys())[:self.levels]
        row = {'time': self.current_time}
        for k in range(self.levels):
            row[f'bid_price_{k+1}'] = bid_prices[k] if k < len(bid_prices) else np.nan
            row[f'bid_size_{k+1}'] = self.bids.get(bid_prices[k], np.nan) if k < len(bid_prices) else np.nan
            row[f'ask_price_{k+1}'] = ask_prices[k] if k < len(ask_prices) else np.nan
            row[f'ask_size_{k+1}'] = self.asks.get(ask_prices[k], np.nan) if k < len(ask_prices) else np.nan
        return row

    def get_best_bid(self):
        """Return (price, size) of the best bid, or (None, 0) if empty."""
        if not self.bids:
            return None, 0
        best_price = max(self.bids.keys())
        return best_price, self.bids[best_price]

    def get_best_ask(self):
        """Return (price, size) of the best ask, or (None, 0) if empty."""
        if not self.asks:
            return None, 0
        best_price = min(self.asks.keys())
        return best_price, self.asks[best_price]

    def get_bid_levels(self):
        """Return (prices, sizes) for bid side, best-first."""
        prices = sorted(self.bids.keys(), reverse=True)[:self.levels]
        sizes = [self.bids[p] for p in prices]
        return np.array(prices), np.array(sizes)

    def get_ask_levels(self):
        """Return (prices, sizes) for ask side, best-first."""
        prices = sorted(self.asks.keys())[:self.levels]
        sizes = [self.asks[p] for p in prices]
        return np.array(prices), np.array(sizes)

    def consume_bids(self, qty):
        """Consume liquidity from the bid side (market sell). Modifies book in place."""
        bid_prices = sorted(self.bids.keys(), reverse=True)
        remaining = qty
        cost = 0.0
        filled = 0.0
        for p in bid_prices:
            if remaining <= 0:
                break
            available = self.bids[p]
            take = min(remaining, available)
            cost += take * p
            filled += take
            remaining -= take
            if take >= available:
                del self.bids[p]
            else:
                self.bids[p] -= take
        if filled > 0:
            return cost / filled, filled
        return np.nan, 0.0

    def consume_asks(self, qty):
        """Consume liquidity from the ask side (market buy). Modifies book in place."""
        ask_prices = sorted(self.asks.keys())
        remaining = qty
        cost = 0.0
        filled = 0.0
        for p in ask_prices:
            if remaining <= 0:
                break
            available = self.asks[p]
            take = min(remaining, available)
            cost += take * p
            filled += take
            remaining -= take
            if take >= available:
                del self.asks[p]
            else:
                self.asks[p] -= take
        if filled > 0:
            return cost / filled, filled
        return np.nan, 0.0


# =====================================================================
# 2. LIMIT ORDER FILL SIMULATOR
# =====================================================================
class LimitOrderFillSimulator:
    """
    Simulates limit order fills by tracking the queue position at a given
    price level and watching for events that would fill our order.
    
    When we place a limit order at the best bid (for a sell) or best ask
    (for a buy), we join the queue at that price level. We get filled when
    the market "consumes" enough of the queue to reach our position.
    """

    def __init__(self, max_wait_ns=MAX_LIMIT_WAIT_NS):
        self.max_wait_ns = max_wait_ns
        self.active_orders = {}  # order_id -> order_info dict

    def place_limit_order(self, order_id, symbol, side, price, quantity,
                          queue_position, current_time):
        """
        Place a simulated limit order.
        
        Parameters
        ----------
        order_id : str
            Unique identifier for this order.
        symbol : str
            'perp' or 'spot'.
        side : str
            'bid' (buy limit) or 'ask' (sell limit).
        price : float
            Limit price.
        quantity : float
            Order quantity in BTC.
        queue_position : int
            Our position in the queue (0 = first in line).
        current_time : pd.Timestamp
            Time the order was placed.
        """
        self.active_orders[order_id] = {
            'symbol': symbol,
            'side': side,
            'price': price,
            'initial_quantity': quantity,
            'remaining_quantity': quantity,
            'filled_quantity': 0.0,
            'fill_price': 0.0,
            'fill_cost': 0.0,
            'queue_position': queue_position,
            'initial_queue_position': queue_position,
            'placed_at': current_time,
            'last_update': current_time,
            'status': 'active',  # active, partially_filled, filled, cancelled
            'fill_events': [],  # list of (time, qty, price) tuples
        }
        return order_id

    def update_queue(self, order_id, new_queue_position):
        """Update the queue position for an active order."""
        if order_id in self.active_orders:
            self.active_orders[order_id]['queue_position'] = new_queue_position

    def process_fill(self, order_id, fill_qty, fill_price, current_time):
        """
        Process a fill event for an active order.
        Returns the amount filled.
        """
        if order_id not in self.active_orders:
            return 0.0

        order = self.active_orders[order_id]
        if order['status'] in ('filled', 'cancelled'):
            return 0.0

        actual_fill = min(fill_qty, order['remaining_quantity'])
        if actual_fill <= 0:
            return 0.0

        order['filled_quantity'] += actual_fill
        order['remaining_quantity'] -= actual_fill
        order['fill_cost'] += actual_fill * fill_price
        order['fill_price'] = order['fill_cost'] / order['filled_quantity']
        order['last_update'] = current_time
        order['fill_events'].append((current_time, actual_fill, fill_price))

        if order['remaining_quantity'] <= 0:
            order['status'] = 'filled'
        else:
            order['status'] = 'partially_filled'

        return actual_fill

    def check_expiry(self, order_id, current_time):
        """
        Check if an order has expired (exceeded max wait time).
        Returns True if expired, False otherwise.
        """
        if order_id not in self.active_orders:
            return True
        order = self.active_orders[order_id]
        time_elapsed = (current_time - order['placed_at']).total_seconds() * 1e9
        return time_elapsed > self.max_wait_ns

    def cancel_order(self, order_id):
        """Cancel an active order. Returns the filled quantity so far."""
        if order_id not in self.active_orders:
            return 0.0
        order = self.active_orders[order_id]
        order['status'] = 'cancelled'
        return order['filled_quantity']

    def get_order_summary(self, order_id):
        """Get a summary of the order's execution."""
        if order_id not in self.active_orders:
            return None
        order = self.active_orders[order_id]
        return {
            'order_id': order_id,
            'symbol': order['symbol'],
            'side': order['side'],
            'price': order['price'],
            'initial_quantity': order['initial_quantity'],
            'filled_quantity': order['filled_quantity'],
            'remaining_quantity': order['remaining_quantity'],
            'avg_fill_price': order['fill_price'] if order['filled_quantity'] > 0 else np.nan,
            'status': order['status'],
            'placed_at': order['placed_at'],
            'last_update': order['last_update'],
            'time_to_first_fill': None,
            'time_to_complete': None,
        }

    def get_active_order_count(self):
        """Return the number of active (non-filled, non-cancelled) orders."""
        return sum(1 for o in self.active_orders.values()
                   if o['status'] not in ('filled', 'cancelled'))


# =====================================================================
# 3. QUEUE POSITION ESTIMATOR
# =====================================================================
def estimate_queue_position(book_side, price, our_qty):
    """
    Estimate our queue position at a given price level.
    
    In a FIFO order book, our position depends on:
    - The total size already at this price level (from existing orders)
    - Our order size
    
    We assume that visible size at the price level is the total queue depth.
    Our position is the existing size (we join at the back of the queue).
    
    Parameters
    ----------
    book_side : dict
        Dictionary of price -> size for the relevant side of the book.
    price : float
        The price level we want to place an order at.
    our_qty : float
        Our order quantity.
    
    Returns
    -------
    queue_position : float
        Estimated position in queue (0 = first in line).
        Higher values = further back in queue.
    total_queue_depth : float
        Total size at this price level before our order.
    """
    total_queue_depth = book_side.get(price, 0.0)
    # Our position is behind all existing orders at this price level
    queue_position = total_queue_depth
    return queue_position, total_queue_depth


def estimate_fill_probability(queue_position, our_qty, total_queue_depth,
                              avg_trade_size, time_horizon_ns=MAX_LIMIT_WAIT_NS,
                              avg_trade_interval_ns=100_000_000):
    """
    Estimate the probability that our limit order gets filled within the
    time horizon, based on queue position and typical market activity.
    
    This is a heuristic based on:
    - How much volume needs to trade before we get filled (queue_position)
    - Typical trade size and frequency
    - Our time horizon
    
    Parameters
    ----------
    queue_position : float
        Our position in the queue (total size ahead of us).
    our_qty : float
        Our order quantity.
    total_queue_depth : float
        Total size at this price level.
    avg_trade_size : float
        Average trade size at this price level.
    time_horizon_ns : int
        How long we're willing to wait (nanoseconds).
    avg_trade_interval_ns : int
        Average time between trades at this price level (nanoseconds).
    
    Returns
    -------
    probability : float
        Estimated fill probability (0.0 to 1.0).
    estimated_trades_needed : int
        Estimated number of trades needed to fill our order.
    """
    # If we're first in line and there's no queue, we'll get filled immediately
    if queue_position <= 0:
        return 1.0, 0

    # Number of trades needed to clear the queue ahead of us
    if avg_trade_size <= 0:
        return 0.0, float('inf')

    trades_needed = queue_position / avg_trade_size
    # Additional trades needed to fill our quantity
    trades_needed += our_qty / avg_trade_size

    # Expected time to get filled
    expected_time_ns = trades_needed * avg_trade_interval_ns

    # Probability based on time horizon
    if expected_time_ns <= 0:
        return 1.0, 0

    prob = min(1.0, time_horizon_ns / expected_time_ns)

    return prob, int(trades_needed)


# =====================================================================
# 4. LIMIT ORDER STRATEGY SIMULATION
# =====================================================================
def simulate_limit_order_strategy(
    df_perp_events,
    df_spot_events,
    df_signals,
    entry_z=2.0,
    qty_grid=QTY_GRID,
    limit_fee_bps=LIMIT_FEE_HURDLE_BPS,
    max_wait_ns=MAX_LIMIT_WAIT_NS,
    max_queue_depth=MAX_QUEUE_DEPTH_BTC,
    levels=N_LEVELS,
):
    """
    Simulate placing limit orders at z-score entry signals.
    
    For each signal:
    1. Check the current best bid/ask on both legs
    2. Estimate queue position at the best price
    3. If queue is not too deep, place a simulated limit order
    4. Track the order as subsequent events arrive
    5. Determine if/when the order gets filled
    6. Record performance metrics
    
    Parameters
    ----------
    df_perp_events : pd.DataFrame
        Perp LOB events in chronological order.
    df_spot_events : pd.DataFrame
        Spot LOB events in chronological order.
    df_signals : pd.DataFrame
        Signal DataFrame with 'time' and 'z_score' columns.
    entry_z : float
        Z-score threshold for entry.
    qty_grid : array-like
        Order sizes to simulate.
    limit_fee_bps : float
        Limit order fee per leg (e.g., 0.0002 = 0.02%).
    max_wait_ns : int
        Maximum time to wait for limit order fill (nanoseconds).
    max_queue_depth : float
        Maximum queue depth we're willing to join (BTC).
    levels : int
        Number of price levels to track.
    
    Returns
    -------
    results : list of dict
        Simulation results for each (signal, qty) pair.
    """
    # Combine and sort events chronologically
    df_perp_events = df_perp_events.copy()
    df_perp_events['symbol'] = 'perp'
    df_spot_events = df_spot_events.copy()
    df_spot_events['symbol'] = 'spot'

    df_all = pd.concat([df_perp_events, df_spot_events], ignore_index=True)
    df_all = df_all.sort_values(['time', 'seq']).reset_index(drop=True)

    # Extract signal times and z-scores
    sig_times = df_signals['time'].values
    sig_zscores = df_signals['z_score'].values

    # Initialize live order books
    perp_book = LiveOrderBook("BTCPERP", levels=levels)
    spot_book = LiveOrderBook("BTCUSDT", levels=levels)

    # Initialize limit order fill simulator
    lo_sim = LimitOrderFillSimulator(max_wait_ns=max_wait_ns)

    # Results storage
    results = []
    active_arb_orders = {}  # signal_time -> {order_id_perp, order_id_spot, ...}

    signal_idx = 0
    n_signals_processed = 0
    n_events_processed = 0
    last_signal_time = None

    print(f"Starting limit order simulation with {len(df_all)} events...")
    print(f"Signal count: {len(sig_times)}")
    print(f"Entry z-score: {entry_z}")

    for event_idx, event in df_all.iterrows():
        n_events_processed += 1
        t = event['time']

        # Update the appropriate book
        if event['symbol'] == 'perp':
            perp_book.apply_event(t, event['side'], event['price'],
                                  event['size'], event['is_snapshot'], event['seq'])
        else:
            spot_book.apply_event(t, event['side'], event['price'],
                                  event['size'], event['is_snapshot'], event['seq'])

        # ---- Check if we've crossed a signal time ----
        while signal_idx < len(sig_times) and sig_times[signal_idx] <= t:
            signal_time = sig_times[signal_idx]
            z_val = sig_zscores[signal_idx]

            if signal_time == last_signal_time:
                signal_idx += 1
                continue

            if abs(z_val) >= entry_z:
                n_signals_processed += 1
                direction = 1 if z_val >= entry_z else -1

                # Get current best prices
                perp_bid, perp_bid_size = perp_book.get_best_bid()
                perp_ask, perp_ask_size = perp_book.get_best_ask()
                spot_bid, spot_bid_size = spot_book.get_best_bid()
                spot_ask, spot_ask_size = spot_book.get_best_ask()

                if None in (perp_bid, perp_ask, spot_bid, spot_ask):
                    signal_idx += 1
                    continue

                for qty in qty_grid:
                    if direction == 1:
                        # Short perp / long spot:
                        #   Sell perp: place limit order at best ASK price (join the ask queue)
                        #   Buy spot:  place limit order at best BID price (join the bid queue)
                        perp_limit_price = perp_ask
                        spot_limit_price = spot_bid

                        # Estimate queue positions on the correct side of the book
                        # Sell limit → joins ASK side; Buy limit → joins BID side
                        perp_qpos, perp_depth = estimate_queue_position(
                            perp_book.asks, perp_limit_price, qty)
                        spot_qpos, spot_depth = estimate_queue_position(
                            spot_book.bids, spot_limit_price, qty)

                        # Skip if queues are too deep
                        if perp_qpos > max_queue_depth or spot_qpos > max_queue_depth:
                            continue

                        # Place simulated limit orders
                        # 'ask' side = sell limit order (offer to sell)
                        # 'bid' side = buy limit order (offer to buy)
                        perp_oid = f"perp_sell_{signal_time}_{qty}_{event_idx}"
                        spot_oid = f"spot_buy_{signal_time}_{qty}_{event_idx}"

                        lo_sim.place_limit_order(
                            perp_oid, 'perp', 'ask', perp_limit_price, qty,
                            perp_qpos, signal_time)
                        lo_sim.place_limit_order(
                            spot_oid, 'spot', 'bid', spot_limit_price, qty,
                            spot_qpos, signal_time)

                        # Estimate fill probability
                        avg_trade_size = min(perp_ask_size, spot_bid_size) * 0.5
                        fill_prob, trades_needed = estimate_fill_probability(
                            max(perp_qpos, spot_qpos), qty,
                            max(perp_depth, spot_depth),
                            max(avg_trade_size, 0.1),
                            max_wait_ns)

                        # Record the entry
                        entry_key = f"{signal_time}_{qty}_{direction}"
                        active_arb_orders[entry_key] = {
                            'perp_oid': perp_oid,
                            'spot_oid': spot_oid,
                            'signal_time': signal_time,
                            'z_val': z_val,
                            'direction': direction,
                            'qty': qty,
                            'perp_limit_price': perp_limit_price,
                            'spot_limit_price': spot_limit_price,
                            'perp_queue_pos': perp_qpos,
                            'spot_queue_pos': spot_qpos,
                            'perp_queue_depth': perp_depth,
                            'spot_queue_depth': spot_depth,
                            'fill_probability': fill_prob,
                            'trades_needed': trades_needed,
                            'perp_bid_at_entry': perp_bid,
                            'perp_ask_at_entry': perp_ask,
                            'spot_bid_at_entry': spot_bid,
                            'spot_ask_at_entry': spot_ask,
                            'perp_bid_size_at_entry': perp_bid_size,
                            'perp_ask_size_at_entry': perp_ask_size,
                            'spot_bid_size_at_entry': spot_bid_size,
                            'spot_ask_size_at_entry': spot_ask_size,
                            'perp_filled': False,
                            'spot_filled': False,
                            'perp_fill_time': None,
                            'spot_fill_time': None,
                            'perp_fill_price': None,
                            'spot_fill_price': None,
                            'perp_fill_qty': 0.0,
                            'spot_fill_qty': 0.0,
                            'cancelled': False,
                            'cancel_time': None,
                        }

                    else:
                        # Long perp / short spot:
                        #   Buy perp:  place limit order at best BID price (join the bid queue)
                        #   Sell spot: place limit order at best ASK price (join the ask queue)
                        perp_limit_price = perp_bid
                        spot_limit_price = spot_ask

                        # Estimate queue positions on the correct side of the book
                        # Buy limit → joins BID side; Sell limit → joins ASK side
                        perp_qpos, perp_depth = estimate_queue_position(
                            perp_book.bids, perp_limit_price, qty)
                        spot_qpos, spot_depth = estimate_queue_position(
                            spot_book.asks, spot_limit_price, qty)

                        # Skip if queues are too deep
                        if perp_qpos > max_queue_depth or spot_qpos > max_queue_depth:
                            continue

                        # Place simulated limit orders
                        # 'bid' side = buy limit order (offer to buy)
                        # 'ask' side = sell limit order (offer to sell)
                        perp_oid = f"perp_buy_{signal_time}_{qty}_{event_idx}"
                        spot_oid = f"spot_sell_{signal_time}_{qty}_{event_idx}"

                        lo_sim.place_limit_order(
                            perp_oid, 'perp', 'bid', perp_limit_price, qty,
                            perp_qpos, signal_time)
                        lo_sim.place_limit_order(
                            spot_oid, 'spot', 'ask', spot_limit_price, qty,
                            spot_qpos, signal_time)

                        # Estimate fill probability
                        avg_trade_size = min(perp_bid_size, spot_ask_size) * 0.5
                        fill_prob, trades_needed = estimate_fill_probability(
                            max(perp_qpos, spot_qpos), qty,
                            max(perp_depth, spot_depth),
                            max(avg_trade_size, 0.1),
                            max_wait_ns)

                        # Record the entry
                        entry_key = f"{signal_time}_{qty}_{direction}"
                        active_arb_orders[entry_key] = {
                            'perp_oid': perp_oid,
                            'spot_oid': spot_oid,
                            'signal_time': signal_time,
                            'z_val': z_val,
                            'direction': direction,
                            'qty': qty,
                            'perp_limit_price': perp_limit_price,
                            'spot_limit_price': spot_limit_price,
                            'perp_queue_pos': perp_qpos,
                            'spot_queue_pos': spot_qpos,
                            'perp_queue_depth': perp_depth,
                            'spot_queue_depth': spot_depth,
                            'fill_probability': fill_prob,
                            'trades_needed': trades_needed,
                            'perp_bid_at_entry': perp_bid,
                            'perp_ask_at_entry': perp_ask,
                            'spot_bid_at_entry': spot_bid,
                            'spot_ask_at_entry': spot_ask,
                            'perp_bid_size_at_entry': perp_bid_size,
                            'perp_ask_size_at_entry': perp_ask_size,
                            'spot_bid_size_at_entry': spot_bid_size,
                            'spot_ask_size_at_entry': spot_ask_size,
                            'perp_filled': False,
                            'spot_filled': False,
                            'perp_fill_time': None,
                            'spot_fill_time': None,
                            'perp_fill_price': None,
                            'spot_fill_price': None,
                            'perp_fill_qty': 0.0,
                            'spot_fill_qty': 0.0,
                            'cancelled': False,
                            'cancel_time': None,
                        }

                last_signal_time = signal_time

            signal_idx += 1

        # ---- Process active orders: check for fills and expiry ----
        expired_entries = []
        for entry_key, entry in active_arb_orders.items():
            if entry['cancelled']:
                continue

            perp_oid = entry['perp_oid']
            spot_oid = entry['spot_oid']
            direction = entry['direction']
            qty = entry['qty']

            # Check if perp order got filled
            # We track cumulative fills: each time the queue moves forward,
            # we fill the amount that became available. Only mark as fully
            # filled when the entire order quantity has been consumed.
            if not entry['perp_filled']:
                perp_order = lo_sim.active_orders.get(perp_oid)
                if perp_order and perp_order['status'] not in ('filled', 'cancelled'):
                    if direction == 1:
                        # Direction = 1 (short perp / long spot):
                        #   We placed a SELL limit on the ASK side at perp_ask price
                        #   Gets filled when a market BUY order lifts our ask
                        #   Condition: best ask is still at our limit price, queue moved forward
                        current_ask, current_ask_size = perp_book.get_best_ask()
                        if current_ask is not None and current_ask >= entry['perp_limit_price']:
                            new_qpos, _ = estimate_queue_position(
                                perp_book.asks, entry['perp_limit_price'], qty)
                            # Calculate how much new volume has been consumed since last check
                            # We track the last known queue position to detect forward movement
                            last_checked_qpos = entry.get('perp_last_qpos', entry['perp_queue_pos'])
                            if new_qpos < last_checked_qpos:
                                # Queue moved forward by (last_checked_qpos - new_qpos)
                                # But we can only fill up to our remaining quantity
                                available_fill = last_checked_qpos - new_qpos
                                remaining_qty = perp_order['remaining_quantity']
                                filled_qty = min(available_fill, remaining_qty)
                                if filled_qty > 0:
                                    lo_sim.process_fill(
                                        perp_oid, filled_qty,
                                        entry['perp_limit_price'], t)
                                    entry['perp_last_qpos'] = new_qpos
                                    # Update fill tracking
                                    updated_order = lo_sim.active_orders.get(perp_oid)
                                    if updated_order and updated_order['remaining_quantity'] <= 0:
                                        entry['perp_filled'] = True
                                        entry['perp_fill_time'] = t
                                        entry['perp_fill_price'] = entry['perp_limit_price']
                                        entry['perp_fill_qty'] = qty
                                    elif updated_order:
                                        # Partial fill - record progress but don't mark as fully filled
                                        entry['perp_fill_price'] = entry['perp_limit_price']
                                        entry['perp_fill_qty'] = updated_order['filled_quantity']
                                else:
                                    entry['perp_last_qpos'] = new_qpos
                            else:
                                entry['perp_last_qpos'] = new_qpos
                    else:
                        # Direction = -1 (long perp / short spot):
                        #   We placed a BUY limit on the BID side at perp_bid price
                        #   Gets filled when a market SELL order hits our bid
                        #   Condition: best bid is still at our limit price, queue moved forward
                        current_bid, current_bid_size = perp_book.get_best_bid()
                        if current_bid is not None and current_bid <= entry['perp_limit_price']:
                            new_qpos, _ = estimate_queue_position(
                                perp_book.bids, entry['perp_limit_price'], qty)
                            last_checked_qpos = entry.get('perp_last_qpos', entry['perp_queue_pos'])
                            if new_qpos < last_checked_qpos:
                                available_fill = last_checked_qpos - new_qpos
                                remaining_qty = perp_order['remaining_quantity']
                                filled_qty = min(available_fill, remaining_qty)
                                if filled_qty > 0:
                                    lo_sim.process_fill(
                                        perp_oid, filled_qty,
                                        entry['perp_limit_price'], t)
                                    entry['perp_last_qpos'] = new_qpos
                                    updated_order = lo_sim.active_orders.get(perp_oid)
                                    if updated_order and updated_order['remaining_quantity'] <= 0:
                                        entry['perp_filled'] = True
                                        entry['perp_fill_time'] = t
                                        entry['perp_fill_price'] = entry['perp_limit_price']
                                        entry['perp_fill_qty'] = qty
                                    elif updated_order:
                                        entry['perp_fill_price'] = entry['perp_limit_price']
                                        entry['perp_fill_qty'] = updated_order['filled_quantity']
                                else:
                                    entry['perp_last_qpos'] = new_qpos
                            else:
                                entry['perp_last_qpos'] = new_qpos

            # Check if spot order got filled
            # Same cumulative fill tracking as perp
            if not entry['spot_filled']:
                spot_order = lo_sim.active_orders.get(spot_oid)
                if spot_order and spot_order['status'] not in ('filled', 'cancelled'):
                    if direction == 1:
                        # Direction = 1 (short perp / long spot):
                        #   We placed a BUY limit on the BID side at spot_bid price
                        #   Gets filled when a market SELL order hits our bid
                        #   Condition: best bid is still at our limit price, queue moved forward
                        current_bid, current_bid_size = spot_book.get_best_bid()
                        if current_bid is not None and current_bid <= entry['spot_limit_price']:
                            new_qpos, _ = estimate_queue_position(
                                spot_book.bids, entry['spot_limit_price'], qty)
                            last_checked_qpos = entry.get('spot_last_qpos', entry['spot_queue_pos'])
                            if new_qpos < last_checked_qpos:
                                available_fill = last_checked_qpos - new_qpos
                                remaining_qty = spot_order['remaining_quantity']
                                filled_qty = min(available_fill, remaining_qty)
                                if filled_qty > 0:
                                    lo_sim.process_fill(
                                        spot_oid, filled_qty,
                                        entry['spot_limit_price'], t)
                                    entry['spot_last_qpos'] = new_qpos
                                    updated_order = lo_sim.active_orders.get(spot_oid)
                                    if updated_order and updated_order['remaining_quantity'] <= 0:
                                        entry['spot_filled'] = True
                                        entry['spot_fill_time'] = t
                                        entry['spot_fill_price'] = entry['spot_limit_price']
                                        entry['spot_fill_qty'] = qty
                                    elif updated_order:
                                        entry['spot_fill_price'] = entry['spot_limit_price']
                                        entry['spot_fill_qty'] = updated_order['filled_quantity']
                                else:
                                    entry['spot_last_qpos'] = new_qpos
                            else:
                                entry['spot_last_qpos'] = new_qpos
                    else:
                        # Direction = -1 (long perp / short spot):
                        #   We placed a SELL limit on the ASK side at spot_ask price
                        #   Gets filled when a market BUY order lifts our ask
                        #   Condition: best ask is still at our limit price, queue moved forward
                        current_ask, current_ask_size = spot_book.get_best_ask()
                        if current_ask is not None and current_ask >= entry['spot_limit_price']:
                            new_qpos, _ = estimate_queue_position(
                                spot_book.asks, entry['spot_limit_price'], qty)
                            last_checked_qpos = entry.get('spot_last_qpos', entry['spot_queue_pos'])
                            if new_qpos < last_checked_qpos:
                                available_fill = last_checked_qpos - new_qpos
                                remaining_qty = spot_order['remaining_quantity']
                                filled_qty = min(available_fill, remaining_qty)
                                if filled_qty > 0:
                                    lo_sim.process_fill(
                                        spot_oid, filled_qty,
                                        entry['spot_limit_price'], t)
                                    entry['spot_last_qpos'] = new_qpos
                                    updated_order = lo_sim.active_orders.get(spot_oid)
                                    if updated_order and updated_order['remaining_quantity'] <= 0:
                                        entry['spot_filled'] = True
                                        entry['spot_fill_time'] = t
                                        entry['spot_fill_price'] = entry['spot_limit_price']
                                        entry['spot_fill_qty'] = qty
                                    elif updated_order:
                                        entry['spot_fill_price'] = entry['spot_limit_price']
                                        entry['spot_fill_qty'] = updated_order['filled_quantity']
                                else:
                                    entry['spot_last_qpos'] = new_qpos
                            else:
                                entry['spot_last_qpos'] = new_qpos

            # Check for expiry
            if not entry['cancelled']:
                perp_expired = lo_sim.check_expiry(perp_oid, t)
                spot_expired = lo_sim.check_expiry(spot_oid, t)

                if perp_expired or spot_expired:
                    # Cancel both orders
                    lo_sim.cancel_order(perp_oid)
                    lo_sim.cancel_order(spot_oid)
                    entry['cancelled'] = True
                    entry['cancel_time'] = t

            # Record result if both filled or cancelled
            both_filled = entry['perp_filled'] and entry['spot_filled']
            if both_filled or entry['cancelled']:
                # Calculate captured spread
                if both_filled:
                    if direction == 1:
                        # Short perp (sold at limit) / Long spot (bought at limit)
                        captured_spread = entry['perp_fill_price'] - entry['spot_fill_price']
                    else:
                        # Long perp (bought at limit) / Short spot (sold at limit)
                        captured_spread = entry['spot_fill_price'] - entry['perp_fill_price']
                else:
                    captured_spread = np.nan

                # Calculate fees (limit order: 0.02% per leg)
                perp_fee = (entry['perp_fill_price'] * qty * limit_fee_bps
                            if entry['perp_filled'] else 0.0)
                spot_fee = (entry['spot_fill_price'] * qty * limit_fee_bps
                            if entry['spot_filled'] else 0.0)
                total_fee = perp_fee + spot_fee

                net_spread = (captured_spread - total_fee
                              if not np.isnan(captured_spread) else np.nan)

                # Time to fill (if both filled)
                if both_filled and entry['perp_fill_time'] and entry['spot_fill_time']:
                    time_to_fill = max(
                        (entry['perp_fill_time'] - entry['signal_time']).total_seconds(),
                        (entry['spot_fill_time'] - entry['signal_time']).total_seconds()
                    )
                else:
                    time_to_fill = np.nan

                results.append({
                    'signal_time': entry['signal_time'],
                    'entry_z': entry['z_val'],
                    'direction': direction,
                    'qty': qty,
                    'method': 'limit_order',
                    'perp_limit_price': entry['perp_limit_price'],
                    'spot_limit_price': entry['spot_limit_price'],
                    'perp_queue_pos': entry['perp_queue_pos'],
                    'spot_queue_pos': entry['spot_queue_pos'],
                    'fill_probability': entry['fill_probability'],
                    'perp_filled': entry['perp_filled'],
                    'spot_filled': entry['spot_filled'],
                    'both_filled': both_filled,
                    'perp_fill_price': entry['perp_fill_price'],
                    'spot_fill_price': entry['spot_fill_price'],
                    'perp_fill_time': entry['perp_fill_time'],
                    'spot_fill_time': entry['spot_fill_time'],
                    'time_to_fill_sec': time_to_fill,
                    'cancelled': entry['cancelled'],
                    'cancel_time': entry['cancel_time'],
                    'captured_spread': captured_spread,
                    'fee_cost': total_fee,
                    'fee_bps': limit_fee_bps * 10000,
                    'net_spread': net_spread,
                    'perp_bid_at_entry': entry['perp_bid_at_entry'],
                    'perp_ask_at_entry': entry['perp_ask_at_entry'],
                    'spot_bid_at_entry': entry['spot_bid_at_entry'],
                    'spot_ask_at_entry': entry['spot_ask_at_entry'],
                    'perp_bid_size_at_entry': entry['perp_bid_size_at_entry'],
                    'perp_ask_size_at_entry': entry['perp_ask_size_at_entry'],
                    'spot_bid_size_at_entry': entry['spot_bid_size_at_entry'],
                    'spot_ask_size_at_entry': entry['spot_ask_size_at_entry'],
                })

                # Remove from active orders
                expired_entries.append(entry_key)

        # Clean up processed entries
        for key in expired_entries:
            if key in active_arb_orders:
                del active_arb_orders[key]

        # Progress indicator
        if n_events_processed % 500000 == 0:
            print(f"  Processed {n_events_processed}/{len(df_all)} events, "
                  f"{n_signals_processed} signals triggered, "
                  f"{len(active_arb_orders)} active orders...")

    print(f"\nSimulation complete. Processed {n_events_processed} events, "
          f"{n_signals_processed} signals triggered.")
    print(f"Results: {len(results)} order pairs recorded.")

    return pd.DataFrame(results)


# =====================================================================
# 5. COMPARISON: LIMIT ORDERS vs MARKET ORDERS
# =====================================================================
def compare_limit_vs_market(df_limit, df_market=None, df_dynamic=None):
    """
    Compare limit order results with market order results.
    
    Parameters
    ----------
    df_limit : pd.DataFrame
        Results from limit order simulation.
    df_market : pd.DataFrame, optional
        Results from static market order simulation (Phase 1).
    df_dynamic : pd.DataFrame, optional
        Results from dynamic market order simulation (Phase 2).
    """
    print("\n" + "=" * 70)
    print("=== LIMIT ORDER vs MARKET ORDER COMPARISON ===")
    print("=" * 70)

    if df_limit.empty:
        print("Limit order results are empty. Nothing to compare.")
        return

    # ---- 1. Overall fill rate ----
    n_total = len(df_limit)
    n_both_filled = df_limit['both_filled'].sum()
    n_cancelled = df_limit['cancelled'].sum()
    n_partial = n_total - n_both_filled - n_cancelled

    print(f"\n--- Limit Order Summary ---")
    print(f"Total signals attempted:     {n_total}")
    print(f"Both legs filled:            {n_both_filled} ({n_both_filled / n_total * 100:.1f}%)")
    print(f"Partially filled:            {n_partial} ({n_partial / n_total * 100:.1f}%)")
    print(f"Cancelled (no fill):         {n_cancelled} ({n_cancelled / n_total * 100:.1f}%)")

    # ---- 2. Fill rate by order size ----
    print(f"\n--- Fill Rate by Order Size ---")
    fill_rate = df_limit.groupby('qty').agg(
        total=('both_filled', 'count'),
        filled=('both_filled', 'sum'),
        fill_rate=('both_filled', 'mean'),
        avg_time_to_fill=('time_to_fill_sec', 'mean'),
        avg_queue_pos=('perp_queue_pos', 'mean'),
    )
    fill_rate['fill_rate_pct'] = fill_rate['fill_rate'] * 100
    print(f"{'Qty':>8} {'Total':>8} {'Filled':>8} {'Fill%':>8} {'Avg TTF(s)':>12} {'Avg QPos':>10}")
    for qty, row in fill_rate.iterrows():
        print(f"{qty:>8.2f} {row['total']:>8.0f} {row['filled']:>8.0f} "
              f"{row['fill_rate_pct']:>7.1f}% {row['avg_time_to_fill']:>11.2f} {row['avg_queue_pos']:>10.2f}")

    # ---- 3. Captured spread comparison (for filled orders only) ----
    df_filled = df_limit[df_limit['both_filled']].copy()
    if not df_filled.empty:
        print(f"\n--- Captured Spread: Limit Orders (filled only) ---")
        spread_stats = df_filled.groupby('qty')['net_spread'].agg(['mean', 'median', 'std', 'count'])
        print(f"{'Qty':>8} {'Count':>8} {'Mean Spread':>14} {'Median Spread':>16} {'Std':>10}")
        for qty, row in spread_stats.iterrows():
            print(f"{qty:>8.2f} {row['count']:>8.0f} {row['mean']:>14.2f} {row['median']:>16.2f} {row['std']:>10.2f}")

    # ---- 4. Comparison with market orders ----
    if df_market is not None and not df_market.empty:
        print(f"\n--- Limit vs Market (Static) Comparison ---")
        df_market_filled = df_market[df_market['depth_exhausted'] == False].copy()
        if not df_market_filled.empty:
            comparison = pd.DataFrame()
            for qty in sorted(set(df_limit['qty'].unique()) & set(df_market_filled['qty'].unique())):
                limit_sub = df_filled[df_filled['qty'] == qty]['net_spread']
                market_sub = df_market_filled[df_market_filled['qty'] == qty]['net_spread']
                if len(limit_sub) > 0 and len(market_sub) > 0:
                    comparison.loc[qty, 'Limit_Mean'] = limit_sub.mean()
                    comparison.loc[qty, 'Market_Mean'] = market_sub.mean()
                    comparison.loc[qty, 'Limit_Median'] = limit_sub.median()
                    comparison.loc[qty, 'Market_Median'] = market_sub.median()
                    comparison.loc[qty, 'Limit_Count'] = len(limit_sub)
                    comparison.loc[qty, 'Market_Count'] = len(market_sub)
                    comparison.loc[qty, 'Diff_Mean'] = limit_sub.mean() - market_sub.mean()

            if not comparison.empty:
                print(f"{'Qty':>8} {'Limit Mean':>12} {'Mkt Mean':>12} {'Diff':>10} "
                      f"{'Limit Med':>12} {'Mkt Med':>12} {'L Count':>8} {'M Count':>8}")
                for qty, row in comparison.iterrows():
                    print(f"{qty:>8.2f} {row['Limit_Mean']:>12.2f} {row['Market_Mean']:>12.2f} "
                          f"{row['Diff_Mean']:>10.2f} {row['Limit_Median']:>12.2f} "
                          f"{row['Market_Median']:>12.2f} {row['Limit_Count']:>8.0f} {row['Market_Count']:>8.0f}")

    # ---- 5. Queue position analysis ----
    print(f"\n--- Queue Position Analysis ---")
    qpos_buckets = [0, 0.1, 0.5, 1.0, 2.0, 5.0, float('inf')]
    labels = ['0-0.1', '0.1-0.5', '0.5-1.0', '1.0-2.0', '2.0-5.0', '5.0+']
    df_limit['qpos_bucket'] = pd.cut(df_limit['perp_queue_pos'],
                                      bins=qpos_buckets, labels=labels)
    qpos_analysis = df_limit.groupby('qpos_bucket', observed=True).agg(
        total=('both_filled', 'count'),
        filled=('both_filled', 'sum'),
        fill_rate=('both_filled', 'mean'),
    )
    qpos_analysis['fill_rate_pct'] = qpos_analysis['fill_rate'] * 100
    print(f"{'Queue Pos':>12} {'Total':>8} {'Filled':>8} {'Fill%':>8}")
    for label in labels:
        if label in qpos_analysis.index:
            row = qpos_analysis.loc[label]
            print(f"{label:>12} {row['total']:>8.0f} {row['filled']:>8.0f} "
                  f"{row['fill_rate_pct']:>7.1f}%")

    # ---- 6. Time-to-fill distribution ----
    if not df_filled.empty:
        print(f"\n--- Time-to-Fill Distribution (seconds) ---")
        ttf = df_filled['time_to_fill_sec'].dropna()
        if len(ttf) > 0:
            print(f"Mean:    {ttf.mean():.3f}s")
            print(f"Median:  {ttf.median():.3f}s")
            print(f"P25:     {ttf.quantile(0.25):.3f}s")
            print(f"P75:     {ttf.quantile(0.75):.3f}s")
            print(f"P90:     {ttf.quantile(0.90):.3f}s")
            print(f"Max:     {ttf.max():.3f}s")
            print(f"Min:     {ttf.min():.3f}s")

    return fill_rate, spread_stats if not df_filled.empty else None


# =====================================================================
# 6. MAIN
# =====================================================================
if __name__ == "__main__":
    print("=" * 70)
    print("PHASE 3: LIMIT ORDER SIMULATION")
    print("=" * 70)
    print(f"\nCommission structure:")
    print(f"  Limit orders:  {LIMIT_FEE_HURDLE_BPS*10000:.2f} bps/leg = {LIMIT_FEE_HURDLE_TOTAL*10000:.2f} bps round-trip")
    print(f"  Market orders: {MARKET_FEE_HURDLE_BPS*10000:.2f} bps/leg = {MARKET_FEE_HURDLE_TOTAL*10000:.2f} bps round-trip")
    print(f"  Limit order max wait: {MAX_LIMIT_WAIT_NS/1e9:.1f}s")
    print(f"  Max queue depth: {MAX_QUEUE_DEPTH_BTC} BTC")

    # Step 1: Load LOB events from database
    engine = create_engine(DB_URL)

    print("\nLoading perp events...")
    df_perp_events = pd.read_sql(f"""
        SELECT time, side, price, size, is_snapshot, seq
        FROM orderbook
        WHERE symbol = 'BTCPERP'
        ORDER BY time ASC, seq ASC
        LIMIT 10000000;
    """, engine)
    # TimescaleDB returns tz-aware timestamps; strip timezone for consistent comparison
    if hasattr(df_perp_events['time'], 'dt') and df_perp_events['time'].dt.tz is not None:
        df_perp_events['time'] = df_perp_events['time'].dt.tz_localize(None)

    print("Loading spot events...")
    df_spot_events = pd.read_sql(f"""
        SELECT time, side, price, size, is_snapshot, seq
        FROM orderbook
        WHERE symbol = 'BTCUSDT'
        ORDER BY time ASC, seq ASC
        LIMIT 10000000;
    """, engine)
    if hasattr(df_spot_events['time'], 'dt') and df_spot_events['time'].dt.tz is not None:
        df_spot_events['time'] = df_spot_events['time'].dt.tz_localize(None)

    print(f"Loaded {len(df_perp_events)} perp events, {len(df_spot_events)} spot events.")

    # Step 2: Build BBO signals (same as Phase 1 & 2)
    print("\nBuilding BBO-level signals...")
    df_perp_depth = reconstruct_book_with_depth("BTCPERP", levels=N_LEVELS)
    df_spot_depth = reconstruct_book_with_depth("BTCUSDT", levels=N_LEVELS)

    # Ensure depth timestamps are also tz-naive
    if hasattr(df_perp_depth['time'], 'dt') and df_perp_depth['time'].dt.tz is not None:
        df_perp_depth['time'] = df_perp_depth['time'].dt.tz_localize(None)
    if hasattr(df_spot_depth['time'], 'dt') and df_spot_depth['time'].dt.tz is not None:
        df_spot_depth['time'] = df_spot_depth['time'].dt.tz_localize(None)

    df_perp_bbo = df_perp_depth.rename(columns={
        'bid_price_1': 'perp_bid', 'bid_size_1': 'perp_bid_size',
        'ask_price_1': 'perp_ask', 'ask_size_1': 'perp_ask_size',
    })[['time', 'perp_bid', 'perp_bid_size', 'perp_ask', 'perp_ask_size']]
    df_spot_bbo = df_spot_depth.rename(columns={
        'bid_price_1': 'spot_bid', 'bid_size_1': 'spot_bid_size',
        'ask_price_1': 'spot_ask', 'ask_size_1': 'spot_ask_size',
    })[['time', 'spot_bid', 'spot_bid_size', 'spot_ask', 'spot_ask_size']]

    df_perp_bbo['perp_time'] = df_perp_bbo['time']
    df_spot_bbo['spot_time'] = df_spot_bbo['time']

    print("Aligning asynchronous data streams for signal generation...")
    df_arb = pd.merge_asof(
        df_perp_bbo[['time', 'perp_time', 'perp_bid', 'perp_bid_size', 'perp_ask', 'perp_ask_size']],
        df_spot_bbo[['time', 'spot_time', 'spot_bid', 'spot_bid_size', 'spot_ask', 'spot_ask_size']],
        on='time',
        direction='backward'
    ).dropna().copy()

    df_signals = prepare_signals(df_arb, window=1000, max_staleness_ms=100, min_vol_usd=1.0)
    # Ensure signal timestamps are tz-naive
    if hasattr(df_signals['time'], 'dt') and df_signals['time'].dt.tz is not None:
        df_signals['time'] = df_signals['time'].dt.tz_localize(None)
    print(f"Generated {len(df_signals)} signals total, "
          f"{len(df_signals[df_signals['z_score'].abs() >= 2.0])} above |z| >= 2.0")

    # Step 3: Run limit order simulation
    print("\n" + "=" * 70)
    print("RUNNING LIMIT ORDER SIMULATION")
    print("=" * 70)

    df_limit = simulate_limit_order_strategy(
        df_perp_events,
        df_spot_events,
        df_signals,
        entry_z=2.0,
        qty_grid=QTY_GRID,
        limit_fee_bps=LIMIT_FEE_HURDLE_BPS,
        max_wait_ns=MAX_LIMIT_WAIT_NS,
        max_queue_depth=MAX_QUEUE_DEPTH_BTC,
        levels=N_LEVELS,
    )

    # Step 4: Save results
    df_limit.to_parquet("data/limit_order_simulation_results.parquet")
    print(f"\nLimit order results saved to data/limit_order_simulation_results.parquet")
    print(f"Saved {len(df_limit)} result rows.")

    # Step 5: Load market order results for comparison
    try:
        df_static = pd.read_parquet("data/liquidity_capacity_results.parquet")
        print(f"\nLoaded static market order results: {len(df_static)} rows")
    except FileNotFoundError:
        print("\nStatic market order results not found. Run liquidity_capacity.py first.")
        df_static = None

    try:
        df_dynamic = pd.read_parquet("data/liquidity_capacity_dynamic.parquet")
        print(f"Loaded dynamic market order results: {len(df_dynamic)} rows")
    except FileNotFoundError:
        print("Dynamic market order results not found. Run liquidity_capacity_dynamic.py first.")
        df_dynamic = None

    # Step 6: Compare
    compare_limit_vs_market(df_limit, df_static, df_dynamic)

    print("\n" + "=" * 70)
    print("SIMULATION COMPLETE")
    print("=" * 70)