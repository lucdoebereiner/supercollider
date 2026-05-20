#!/usr/bin/env bash
# Build and run the standalone demo (no SuperCollider tree required).
set -euo pipefail
cd "$(dirname "$0")"

echo "== cargo test =="
( cd sc-prim && cargo test )

echo
echo "== cargo build --release (libsc_prim.a) =="
( cd sc-prim && cargo build --release )

echo
echo "== build mock host =="
mkdir -p build
g++ -std=c++17 -Wall -Ihost mock_host/mock_host.cpp \
    -Wl,--start-group sc-prim/target/release/libsc_prim.a -Wl,--end-group \
    -lpthread -ldl -lm -o build/mock_demo

echo
echo "== run demo =="
./build/mock_demo
