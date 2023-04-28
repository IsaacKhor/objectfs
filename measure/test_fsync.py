import os
import time

if __name__ == '__main__':
    path = '/mnt/fsbench/test.txt'
    fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_TRUNC)

    for i in range(100):
        os.write(fd, b'hello world\n')

        # measure time elapsed for following fsync
        ts = time.perf_counter_ns()
        os.fsync(fd)
        te = time.perf_counter_ns()

        print(f'fsync time: {te - ts} ns')
