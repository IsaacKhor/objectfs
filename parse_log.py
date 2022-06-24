import os
import pandas as pd
from IPython.display import display

def main():
    data = []
    lines = open('log.txt', 'r').readlines()
    for line in lines:
        pairs = line.strip().split(',')
        row = []
        for pair in pairs:
            row.append(tuple([entry.strip() for entry in pair.split(':')]))

        data.append(dict(row))
    
    df = pd.DataFrame(data=data)    
    display(df)
    df.to_csv('log.csv')

if __name__ == "__main__":
    main()
            