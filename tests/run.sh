#!/bin/sh
# make test: the engine checks, then the test pages through cyb_shot (their
# pictures land in build/shots to look at).
set -e
cd "$(dirname "$0")/.."
./build/test_engine
mkdir -p build/shots
for p in text images; do
    ./build/cyb_shot --keys "down,down,shot:build/shots/$p-2.png,right,right" build/shots/$p.png "file://$PWD/tests/pages/$p.html"
done
echo "pages shown: build/shots"
