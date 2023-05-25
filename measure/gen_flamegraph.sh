#!/bin/bash

set -euo pipefail

cd ../code
sudo chown isaackhor: perf.data
perf script > out.perf
mv out.perf ~/code/FlameGraph/
cd ~/code/FlameGraph/
./stackcollapse-perf.pl out.perf > perf.data.folded
./flamegraph.pl perf.data.folded > perf.svg
cp perf.svg ~/code/obfs/
