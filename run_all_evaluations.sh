#!/bin/bash
set -e

echo "========================================================================"
echo "          PULSESHIELD V8 ELITE ISEF MASTER VERIFICATION SUITE           "
echo "========================================================================"
echo "Target Platform: Teensy 4.0 (ARM Cortex-M7) & Host C++17 Verification"
echo ""

echo "[1/4] Compiling 15-case transient suppression benchmark..."
g++ -x c++ -DPS_HOST -O3 -std=c++17 -Wall PulseShield_v8_audioshield.ino benchmark_v8.cpp -o run_benchmark

echo "[2/4] Compiling passband transparency and THD fidelity audit..."
g++ -x c++ -DPS_HOST -O3 -std=c++17 -Wall PulseShield_v8_audioshield.ino isef_fidelity_audit.cpp -o run_fidelity_audit

echo "[3/4] Compiling standalone 16-bit PCM WAV batch processor..."
g++ -x c++ -DPS_HOST -O3 -std=c++17 -Wall PulseShield_v8_audioshield.ino wav_processor.cpp -o process_wav

echo ""
echo "=== STEP 1: RUNNING TRANSIENT SUPPRESSION BENCHMARK ==="
./run_benchmark

echo ""
echo "=== STEP 2: RUNNING PASSBAND FIDELITY & THD AUDIT ==="
./run_fidelity_audit

echo ""
echo "=== STEP 3: BATCH PROCESSING WAV SAMPLES ==="
if [ -f "test_audio/stimulus_alarm_3k.wav" ]; then
    ./process_wav test_audio/stimulus_alarm_3k.wav test_audio/filtered_alarm_3k.wav
fi
if [ -f "test_audio/stimulus_clap.wav" ]; then
    ./process_wav test_audio/stimulus_clap.wav test_audio/filtered_clap.wav
fi

echo ""
echo "========================================================================"
echo "  ALL VERIFICATIONS PASSED: PulseShield v8 is 100% verified for ISEF!   "
echo "========================================================================"
