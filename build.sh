#!/bin/bash
set -e

echo "=== Building PulseShield v8-Elite DSP Host Suite ==="
OS="$(uname -s)"

# 1. Compile host shared library for Python ctypes (run_battery.py, figures.py)
if [ "$OS" = "Darwin" ]; then
    echo "Compiling libpulseshield.dylib (macOS)..."
    g++ -x c++ -DPS_HOST -O3 -std=c++17 -Wall -shared -fPIC PulseShield_v8_audioshield.ino -o libpulseshield.dylib
else
    echo "Compiling libpulseshield.so (Linux)..."
    g++ -x c++ -DPS_HOST -O3 -std=c++17 -Wall -shared -fPIC PulseShield_v8_audioshield.ino -o libpulseshield.so
fi

# 2. Compile standalone verification binaries
echo "Compiling run_benchmark..."
g++ -x c++ -DPS_HOST -O3 -std=c++17 -Wall PulseShield_v8_audioshield.ino benchmark_v8.cpp -o run_benchmark

echo "Compiling run_fidelity_audit..."
g++ -x c++ -DPS_HOST -O3 -std=c++17 -Wall PulseShield_v8_audioshield.ino isef_fidelity_audit.cpp -o run_fidelity_audit

echo "Compiling process_wav..."
g++ -x c++ -DPS_HOST -O3 -std=c++17 -Wall PulseShield_v8_audioshield.ino wav_processor.cpp -o process_wav

echo "=== Build Complete: All binaries and shared libraries ready ==="
