import pandas as pd
from pathlib import Path

data_dir = Path('DeepMarket/ABIDES/research/data')
print('=== Available Parquet Files ===')
parquet_files = sorted(data_dir.glob('*.parquet'))
for f in parquet_files:
    size_mb = f.stat().st_size / (1024*1024)
    print(f'  {f.name}: {size_mb:.2f} MB')

print('\n=== Combined File Analysis ===')
combined = data_dir / 'limit_order_all_qtys.parquet'
if combined.exists():
    df = pd.read_parquet(combined)
    print(f'Total records: {len(df)}')
    print(f'Columns: {list(df.columns)}')
    if 'qty' in df.columns:
        print(f'Unique quantities: {sorted(df["qty"].unique())}')
    if 'both_filled' in df.columns:
        print(f'Fill rate: {df["both_filled"].mean()*100:.2f}%')
    print(f'\nFirst few rows:')
    print(df.head())
else:
    print('Combined file not found')