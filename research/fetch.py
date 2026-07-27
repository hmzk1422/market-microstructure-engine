import pandas as pd
from sqlalchemy import create_engine

# Connection string matching your Docker Compose setup
# Format: postgresql://user:password@host:port/database_name
DB_URL = "postgresql://market:market@localhost:5432/market_data"

def fetch_market_data():
    # Create the SQLAlchemy engine
    engine = create_engine(DB_URL)

    # Example query to fetch recent trades from the table defined in your engine[cite: 1]
    query = """
        SELECT time, symbol, price, size, side, is_buyer_maker 
        FROM trades 
        ORDER BY time DESC 
        LIMIT 100;
    """

    # Read data directly into a Pandas DataFrame
    df = pd.read_sql(query, engine)
    
    print(f"Successfully fetched {len(df)} rows from TimescaleDB.")
    print(df.head())

    return df

if __name__ == "__main__":
    df = fetch_market_data()