import json
import re

# Read the notebook
with open('../research/ttr_decay_zscores.ipynb', 'r') as f:
    nb = json.load(f)

# Extract code from cells with economic backtest
code_cells = []
for cell in nb['cells']:
    if cell['cell_type'] == 'code' and cell.get('source'):
        source = ''.join(cell['source'])
        if 'run_economic_backtest' in source or 'kaplan_meier' in source:
            code_cells.append(source)

# Combine and save
combined_code = '\n\n'.join(code_cells)
with open('empirical_decay_extracted.py', 'w') as f:
    f.write(combined_code)

print(f'Extracted {len(code_cells)} code cells')
print('Saved to empirical_decay_extracted.py')