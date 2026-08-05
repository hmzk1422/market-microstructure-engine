with open('FinancialAgent.py', 'r') as f:
    content = f.read()
content = content.replace('return ".format(cents / 100)', 'return "".format(cents / 100)')
content = content.replace('return ".format(cents)', 'return "".format(cents)')
with open('FinancialAgent.py', 'w') as f:
    f.write(content)
print("Fixed!")
