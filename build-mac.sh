#!/bin/sh
# Builds bin/zrawdng, the converter for Intel Macs (x86-64, macOS 10.15 or later). It runs on the CPU
# only and needs AVX2, which every Intel Mac from 2013 on has. Needs Apple's command line tools
# (xcode-select --install). Not yet tested on a Mac.
# No -ffast-math or -mfma: the lossy DCT must give the same bits as the Windows build.
set -e
cd "$(dirname "$0")"
mkdir -p bin
clang++ -arch x86_64 -mmacosx-version-min=10.15 -O3 -std=c++17 -mavx2 -mbmi -mbmi2 -mlzcnt -pthread -ffp-contract=off -Ithird_party engine/main.cpp -o bin/zrawdng
echo "Built bin/zrawdng"
