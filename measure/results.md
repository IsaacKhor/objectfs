# Filebench results

OBFS fileserver:

```text
72.034: Run took 60 seconds...
72.037: Per-Operation Breakdown
statfile1            9828ops      164ops/s   0.0mb/s    0.295ms/op [0.027ms - 14.198ms]
deletefile1          9827ops      164ops/s   0.0mb/s    0.679ms/op [0.052ms - 22.214ms]
closefile3           9828ops      164ops/s   0.0mb/s    0.015ms/op [0.001ms - 3.116ms]
readfile1            9828ops      164ops/s  20.5mb/s  295.767ms/op [0.142ms - 2242.855ms]
openfile2            9878ops      165ops/s   0.0mb/s    0.274ms/op [0.043ms - 8.318ms]
closefile2           9878ops      165ops/s   0.0mb/s    0.007ms/op [0.001ms - 0.214ms]
appendfilerand1      9878ops      165ops/s   1.3mb/s    0.501ms/op [0.001ms - 20.580ms]
openfile1            9878ops      165ops/s   0.0mb/s    0.310ms/op [0.047ms - 11.138ms]
closefile1           9878ops      165ops/s   0.0mb/s    0.014ms/op [0.001ms - 1.424ms]
wrtfile1             9878ops      165ops/s  20.4mb/s    4.936ms/op [0.050ms - 54.064ms]
createfile1          9878ops      165ops/s   0.0mb/s    0.642ms/op [0.085ms - 30.332ms]
72.037: IO Summary: 108457 ops 1807.062 ops/s 164/329 rd/wr  42.2mb/s 27.500ms/op
```

S3FS fileserver:

```text
173.284: Run took 60 seconds...
173.285: Per-Operation Breakdown
statfile1            3702ops       62ops/s   0.0mb/s   37.332ms/op [0.004ms - 160.688ms]
deletefile1          3704ops       62ops/s   0.0mb/s  118.274ms/op [0.990ms - 566.963ms]
closefile3           3711ops       62ops/s   0.0mb/s   28.159ms/op [0.032ms - 135.900ms]
readfile1            3715ops       62ops/s   7.6mb/s   41.013ms/op [0.787ms - 231.425ms]
openfile2            3717ops       62ops/s   0.0mb/s  102.018ms/op [13.946ms - 307.189ms]
closefile2           3731ops       62ops/s   0.0mb/s   34.887ms/op [3.599ms - 154.362ms]
appendfilerand1      3732ops       62ops/s   0.5mb/s   45.422ms/op [0.023ms - 207.653ms]
openfile1            3737ops       62ops/s   0.0mb/s  105.438ms/op [10.859ms - 352.447ms]
closefile1           3743ops       62ops/s   0.0mb/s   35.264ms/op [2.746ms - 157.732ms]
wrtfile1             3744ops       62ops/s   7.7mb/s   40.076ms/op [0.039ms - 245.455ms]
createfile1          3745ops       62ops/s   0.0mb/s  215.086ms/op [5.156ms - 1132.812ms]
173.285: IO Summary: 40981 ops 682.971 ops/s 62/125 rd/wr  15.8mb/s 73.048ms/op
```

## Random write

OBFS:

```text
rand-write1          678409ops    11306ops/s  88.3mb/s    0.086ms/op [0.063ms - 189.263ms]
109.497: IO Summary: 678409 ops 11306.054 ops/s 0/11306 rd/wr  88.3mb/s 0.086ms/op
```

S3FS:

```text
rand-write1          19677ops      328ops/s   2.6mb/s    3.043ms/op [0.028ms - 217.297ms]
77.381: IO Summary: 19677 ops 327.915 ops/s 0/328 rd/wr   2.6mb/s 3.043ms/op
```
