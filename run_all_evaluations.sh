#!/bin/bash
set -e

echo "========================================================================"
echo "          PULSESHIELD V8 ELITE ISEF MASTER VERIFICATION SUITE           "
echo "========================================================================"
echo "Target Platform: Teensy 4.0 (ARM Cortex-M7) & Host C++17 Verification"
echo ""

echo "[1/6] Running master build..."
./build.sh

echo ""
echo "[2/6] Running 15-case C++ transient regression suite..."
./run_benchmark

echo ""
echo "[3/6] Running passband transparency and THD fidelity audit..."
./run_fidelity_audit

echo ""
echo "[4/6] Running 180-clip standardized evaluation battery (battery_raw.csv)..."
python3 run_battery.py

echo ""
echo "[5/6] Running non-event control stimuli & false-trigger immunity audit..."
python3 run_extra.py

echo ""
echo ""
echo "[6/7] Generating publication-quality waveforms and response plots..."
python3 figures.py

echo ""
echo "[7/7] Running in silico LQT2 cardiac cellular electrophysiology simulation..."
python3 simulate_lqt2_cardiac.py

echo ""
echo "=== BATCH PROCESSING SAMPLE WAV FILES ==="
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
