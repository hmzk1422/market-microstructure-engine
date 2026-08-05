"""Profile the crypto_limit_order_sim to find the hot spots."""
import cProfile
import pstats
import sys
import os
import io

# Run the sim via its main with a small event limit that produces signals
sys.argv = ['crypto_limit_order_sim.py', '--qty', '1.0', '--event-limit', '5000']

profiler = cProfile.Profile()
profiler.enable()

# Import and run main
from config.crypto_limit_order_sim import main
main()

profiler.disable()

# Print top 40 functions by cumulative time
s = io.StringIO()
ps = pstats.Stats(profiler, stream=s).sort_stats('cumulative')
ps.print_stats(40)
print(s.getvalue())