import pandas as pd
import numpy as np

def generate_features(df):
    df = df.copy()

def generate_target(df):
    df = df.copy()
    df['target'] = df['price'].shift(-1)

    return df