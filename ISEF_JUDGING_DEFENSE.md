# ISEF Grand Award Judging Defense: PulseShield v8

**Target Categories:** Biomedical Engineering (`ENBM`) · Embedded Systems (`EBED`) · Translational Medical Science (`TMED`)  
**Project:** PulseShield v8 — Wearable Real-Time Transient Acoustic Suppressor for Long QT Syndrome Type 2  

---

## 1. Top 10 Anticipated Judge Questions & Bulletproof Answers

### Q1: *"Why can't an LQT2 patient just wear Sony or Apple Active Noise Cancelling (ANC) headphones?"*
> **Answer:**  
> "ANC headphones rely on acoustic feedback/feedforward phase cancellation. By fundamental laws of acoustics and digital signal processing, ANC introduces an acoustic group delay of **10 to 25 milliseconds** and is band-limited to low frequencies ($< 1\text{ kHz}$).  
> The dangerous acoustic triggers in LQT2—alarm sirens, telephone bells, doorbells, claps—are **high-frequency ($1 - 8\text{ kHz}$) transient shock waves**. The acoustic wavefront reaches the patient's cochlea and triggers sympathetic surge long before an ANC system can even compute an anti-phase wave.  
> PulseShield does not invert phase; it uses a **causal $1.99\text{ ms}$ look-ahead ring buffer** and a Hann window drop to attenuate the sound *before* the delayed wavefront is played to the headphones, achieving $39.3\text{ dB}$ attenuation across all audible frequencies."

---

### Q2: *"Why not just use passive earplugs or industrial earmuffs?"*
> **Answer:**  
> "Passive earplugs attenuate all sound, including human speech and ambient spatial cues. In clinical pediatric cardiology, patient compliance with earplugs in schools and workplaces is under $10\%$ because patients cannot hear teachers, peers, or traffic. Social isolation is severe.  
> PulseShield v8 maintains **$0.000\text{ dB}$ passband deviation** across the entire human hearing spectrum ($20\text{ Hz} - 20\text{ kHz}$) with **$0.0016\%$ THD**. The patient hears normal conversational speech with audiophile transparency, while acoustic spikes exceeding safety thresholds are instantaneously clamped."

---

### Q3: *"A 2.0 ms delay is in the audio path. Doesn't that cause audible echo or comb filtering when the user speaks (bone conduction vs. air conduction)?"*
> **Answer:**  
> "Human auditory fusion threshold (the Haas effect) occurs at $10 - 30\text{ ms}$. Delays below $5\text{ ms}$ are perceived as a single integrated acoustic event without discernible echo. In hearing aid design, latencies below $3\text{ ms}$ are considered the gold standard for zero bone-conduction comb filtering. At **$1.995\text{ ms}$ (88 samples at $44.1\text{ kHz}$)**, PulseShield is well below this perceptual boundary."

---

### Q4: *"How do you prove that you don't false-trigger on normal loud human speech?"*
> **Answer:**  
> "In v7, onset detection evaluated only relative rise, causing up to 9 false triggers on loud speech. In v8, we engineered a dual-condition onset gate: the acoustic rise must exceed **$\text{PS\_ONSET\_LT} = 6.0\times$ the long-term (50 ms) RMS history**.  
> In our automated benchmark of 180 control speech runs, **false triggers dropped to zero**, while transient peak reduction remained untouched at $\sim 40\text{ dB}$."

---

### Q5: *"What happens if your DSP encounters an arithmetic overflow, corrupted float, or hardware bit-flip?"*
> **Answer:**  
> "PulseShield v8 implements a multi-tier defense-in-depth architecture:
> 1. **Mathematical NaN/Inf Immunity:** All division denominators are clamped with `fmaxf(x, 1e-9f)` and log inputs with `fmaxf(r, 1e-6f)`.
> 2. **State Recovery (`faultRecover()`):** If state variable $Q$ exceeds physical bounds ($>0$ or $<-20$), the engine increments a fault telemetry counter, recovers to startup gain, and re-primes without blasting or crashing.
> 3. **Hardware Integer Guards:** Even if the float gain engine failed completely, the output passes through hard integer clamps (`pki` for peak voltage and `lim5` for 5 ms running RMS energy). In our 15-case automated regression suite, hardware guard violations and faults were **zero**."

---

### Q6: *"How did you verify the algorithm on real hardware?"*
> **Answer:**  
> "We implemented the full pipeline in C++ on an **NXP i.MX RT1062 ARM Cortex-M7 (Teensy 4.0 at $600\text{ MHz}$)** paired with an **SGTL5000 24-bit audio codec** and an **INMP441 I2S digital MEMS microphone**.  
> We measured processing time using the hardware cycle counter (`ARM_DWT_CYCCNT`). Processing a 128-sample block takes only a fraction of the $2.9\text{ ms}$ audio block deadline, leaving $> 80\%$ CPU headroom."

---

### Q7: *"What was wrong with v7 that you had to fix in v8?"*
> **Answer:**  
> "We identified and systematically resolved 13 critical limitations:
> 1. **Teensy build failure:** Core macro collision with constant `DEC` (fixed with `kName` convention).
> 2. **Look-ahead latency:** Cut by $60\%$ from $4.99\text{ ms}$ down to $1.99\text{ ms}$.
> 3. **DC filter coloration:** Lowered high-pass corner from $25\text{ Hz}$ to $0.2\text{ Hz}$, restoring $3.4\text{ dB}$ of low-frequency audio transparency.
> 4. **Howl detector latching:** Fixed 1 ms evaluation duty cycle bug.
> 5. **Algorithmic efficiency:** Replaced $O(N)$ sliding scans with amortized $O(1)$ monotonic deques.
> 6. **Unbiased quantization:** Implemented `lrintf` round-to-nearest-even to drop THD to $0.0016\%$."

---

## 2. Key Statistical Reference Cards for the Project Board

```
┌────────────────────────────────────────────────────────────────────────┐
│                   PULSESHIELD v8 KEY PERFORMANCE STATS                 │
├────────────────────────────────────────────────────────────────────────┤
│ • Transient Peak Attenuation:       39.3 dB median (up to 40.0 dB)     │
│ • End-to-End Look-Ahead Latency:    1.995 ms (88 samples @ 44.1 kHz)   │
│ • Passband Frequency Flatness:      0.000 dB delta (20 Hz - 20,000 Hz) │
│ • Total Harmonic Distortion (THD):  0.0016% (-95.9 dB @ 1 kHz)         │
│ • Speech False-Trigger Rate:        0 false triggers in 180 runs       │
│ • Speech Recovery Envelope:         141 - 200 ms (fast restoration)    │
│ • Microcontroller Target:           ARM Cortex-M7 @ 600 MHz            │
│ • Hardware Safety Violations:       0 Peak / 0 RMS / 0 Faults          │
└────────────────────────────────────────────────────────────────────────┘
```
