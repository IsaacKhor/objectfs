import os
import pandas as pd
from IPython.display import display

def main():
    data = []
    data_2 = []
    lines = open('/mnt/ramdisk/log.txt', 'r').readlines()

    s3_put = False
    maybe_write = False
    write_everything_out = False
    fs_write = False
    times = {"maybe_write": 0, "write_everything_out": 0, "s3_put": 0, "make_record": 0}
    #times_2 = {"first": 0, "make_record": 0, "maybe_write": 0}
    for line in lines:
        pairs = line.strip().split(';')
        #print(line)
        if "FS_WRITE" in line:
            times["fs_write"] = float(pairs[0].split(":")[1].strip()) #- times["s3_put"] - times["write_everything_out"] - times["maybe_write"] - times["make_record"]
            data.append(times)
            times = {"maybe_write": 0, "write_everything_out": 0, "s3_put": 0, "make_record": 0}
            continue
        elif "BEFORE_MAKE_RECORD" in line:
            times["before_make_record"] = float(pairs[0].split(":")[1].strip())
            continue
        elif "BEFORE_MAYBE_WRITE" in line:
            times["before_maybe_write"] = float(pairs[0].split(":")[1].strip())
            continue
        elif "MAKE_RECORD:" in line:
            times["make_record"] = float(pairs[0].split(":")[1].strip())
            continue
        elif "MAYBE_WRITE:" in line:
            times["maybe_write"] = float(pairs[0].split(":")[1].strip()) #- times["s3_put"] - times["write_everything_out"]
            continue
        elif "WRITE_EVERYTHING_OUT" in line:
            times["write_everything_out"] = float(pairs[0].split(":")[1].strip()) #- times["s3_put"]
            continue
        elif "S3_PUT" in line:
            times["s3_put"] = float(pairs[0].split(":")[1].strip())
            continue
       
    df = pd.DataFrame(data=data)    
    display(df)
    df.to_csv('log/log.csv')

if __name__ == "__main__":
    main()
'''
        row = []
        for pair in pairs:
            row.append(tuple([entry.strip() for entry in pair.split(':')]))
        #print(line)
        row = dict(row)
        op = row['Function']
        if op == "fs_write":
            times["fs_write"] = float(row["Time"]) - times["maybe_write"] - times["make_record"] - times["write_everything_out"] - times["s3_put"]
            #print(times["make_record"])
            #data.append(times)
            fs_write = False
            
        elif op == "maybe_write" and fs_write:
            times["maybe_write"] = float(row["Time"]) - times["write_everything_out"] - times["s3_put"]
        elif op == "write_everything_out" and fs_write:
            times["write_everything_out"] = float(row["Time"]) - times["s3_put"]
        elif op == "s3_put" and fs_write:
            times["s3_put"] = float(row["Time"])
        elif op == "make_record" and fs_write:
            times["make_record"] = float(row["Time"])
            #print(times["make_record"])
'''
        #else:
            #times = {}
            #times = {"maybe_write": 0, "write_everything_out": 0, "s3_put": 0, "make_record": 0}
    

            
