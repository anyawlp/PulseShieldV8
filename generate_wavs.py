import numpy as np
import scipy.io.wavfile as wavfile
import subprocess
import os

print("=== Generating Synthetic Test Suite for PulseShield v8 ===")
os.makedirs("test_audio", exist_ok=True)
sr = 44100
dur = 2.5
t = np.linspace(0, dur, int(sr * dur), endpoint=False)

def write_wav(filename, data):
    data = np.clip(data, -1.0, 1.0)
    int16_data = (data * 32767).astype(np.int16)
    wavfile.write(os.path.join("test_audio", filename), sr, int16_data)

# 1. Loud alarm transient at 1.0s
bg = 0.01 * np.random.randn(len(t)) # -40 dBFS speech-like background
alarm = np.zeros_like(t)
idx = (t >= 1.0) & (t <= 1.05)
alarm[idx] = 0.90 * np.sin(2 * np.pi * 3000 * t[idx])
write_wav("stimulus_alarm_3k.wav", bg + alarm)

# 2. Clap impulse at 1.0s
clap = np.zeros_like(t)
idx = (t >= 1.0) & (t <= 1.05)
clap[idx] = 0.90 * np.exp(-(t[idx] - 1.0) * 300) * np.random.randn(np.sum(idx))
write_wav("stimulus_clap.wav", bg + clap)

print("Generated test_audio/stimulus_alarm_3k.wav and stimulus_clap.wav successfully.")
