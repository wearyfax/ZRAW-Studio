#!/bin/sh
# Builds bin/zrawdng, the converter for Linux x86-64. It runs on the CPU only and needs AVX2 (every
# such CPU also has BMI and LZCNT). Tested with g++ 13.
# No -ffast-math or -mfma: the lossy DCT must give the same bits as the Windows build.
set -e
cd "$(dirname "$0")"
mkdir -p bin
g++ -O3 -std=c++17 -mavx2 -mbmi -mbmi2 -mlzcnt -pthread -ffp-contract=off -static-libstdc++ -static-libgcc -Ithird_party engine/main.cpp -o bin/zrawdng
echo "Built bin/zrawdng"
