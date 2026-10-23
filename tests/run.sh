#!/bin/sh
# make test: the engine checks, the test pages through cyb_shot (their
# pictures land in build/shots to look at) and the WAP gate.
set -e
cd "$(dirname "$0")/.."
./build/test_engine
mkdir -p build/shots
for p in text images; do
    ./build/cyb_shot --keys "down,down,shot:build/shots/$p-2.png,right,right" build/shots/$p.png "file://$PWD/tests/pages/$p.html"
done
# the WAP gate: a page as WML, then its form sent through the gate
./build/test_gate -q "file://$PWD/tests/pages/text.html" "http://c/f3?v0=Bob&v5=a"
./build/test_gate -q -w http://c/wifi http://c/j3 "http://c/f4?v0=pa%24s" http://c/x5 http://c/wifi?scan
# the same for the Browser app: pages laid out, and a long page in parts
./build/test_gate -q -a -w http://c/ "file://$PWD/tests/pages/text.html" "http://c/f3?v0=Bob&v3=2&v5=a" http://c/wifi
./build/test_gate -q -a "file://$PWD/tests/pages/long.html" http://c/y1 http://c/y2
./build/test_gate -q -p "file://$PWD/tests/pages/images.html"
echo "pages shown: build/shots"
