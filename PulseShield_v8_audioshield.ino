// PulseShield v8 -> Teensy 4.0 (C++), per-sample, fully causal, bounded work per sample.
// STATUS: experimental transient suppressor + level limiter. NOT a validated hearing-protection or medical device.
//   All level thresholds are normalized digital full-scale units; they must be calibrated to dB SPL for the whole chain.
//
// WHAT CHANGED vs v7 (regression-tested by eval_v8.cpp, host build with -DPS_HOST):
//  1. BUILD FIX: v7 declared a constant named DEC. The Arduino/Teensy core defines DEC as a macro (=10, used by
//     Serial.print(x, DEC)), so on the real toolchain that line does not compile. All constants are now kName style and
//     cannot collide with core macros.
//  2. Howl detector latch: the howl test was evaluated once per ms but the flag was raised on every sample, so on 43 of 44
//     samples it fed a default baseline instead of the measured one. The measured baseline/gain need is now latched
//     for the whole ms.
//  3. DC removal: leaky DC estimate with a low corner (PS_DC_HZ), primed on the first sample and warmed in as a running
//     mean (v7: 25 Hz corner, which cut low-frequency content of "unity" controls by up to 3.4 dB; and a start-up step
//     of the whole mic DC offset). v8 default corner is 0.2 Hz (below the audible band, still tracks mic DC drift).
//  4. Exact unity: when the whole Hann window of the plan is 1.0, gain is set to exactly 1.0f. The float sum of the
//     kernel taps could land at 0.9999999, truncating every output sample by 1 LSB.
//  5. Sliding max/min use monotonic deques (amortised O(1)); v7 scanned 44 + 89 values every sample. Output is identical.
//  6. A missing input block is replaced by silence and processed, so the delay line and detectors stay time-aligned.
//     A block that cannot be allocated at all is now counted (dropped) instead of vanishing silently.
//  7. Telemetry measures the real DSP time per block with the CPU cycle counter. AudioProcessorUsage() only counts work
//     inside AudioStream::update() (v7 did its DSP in loop(), so its 0.03% figure did not include the filter).
//  8. Non-finite/out-of-range state recovers to the start-up gain and counts a fault instead of muting or blasting.
//  9. Dead code removed (unused PS_USE_QUEUES, a no-op statement). Compile-time checks on every configuration constant,
//     including "event cap <= always-on RMS ceiling".
//  9b. Onset gate: the rise must also exceed PS_ONSET_LT (6x) the long-term RMS, up from 3x. With 3x the 5/10 ms scales
//     fired on peaks of noise-like speech/loud-pink controls (up to 9 false events in 12 control runs, gain dips of
//     several dB). With 6x: 0 false triggers in 180 control runs; event peak reduction unchanged to within ~1 dB.
// 10. RMS bound has a 1% margin (rWr). With zero margin, sustained full-scale square/Nyquist input made the integer RMS
//     guard fire thousands of times (float vs rounded-sample slack). Now the guards are a true backstop (0 in tests).
//
// 11. BOUND FIX: the delay line stored the unclipped DC-removed sample (xs) while every bound (peak, RMS) is computed
//     from the clipped, rounded sample (xi). With large DC steps/wander |xs| can exceed 32767, so the output could be
//     louder than the bound the gain was planned for (the int16 guards then had to catch it). The delay line now stores xi,
//     so the gain bounds apply to exactly the samples that are played.
// 12. START-UP RACE FIX: the audio interrupt could call step() while setup() was still running reset(). reset() now runs
//     before AudioMemory(), and update() outputs silence until setup() has finished ("ready" flag).
// 13. Codec writes are verified by read-back and retried; a mismatch is printed instead of silently ignored.
//
// ALWAYS-ON PROTECTION (independent of the onset/howl detectors): output peak <= PS_SAFE_PEAK and 5 ms output RMS
// <= PS_SAFE_RMS at all times, including start-up (gain held at PS_START_GAIN until the history is full), slow ramps and
// sustained loud input. The detectors only add faster/lower event-specific caps (PS_ABS_CAP) on top of that ceiling.
// Hardware: INMP441 L/R -> GND (left). Shield codec ADC / I2S data-out disabled so pin 8 carries the INMP441.

#define PS_HP_VOLUME 0.25f          // headphone volume 0..1. Start LOW.
#define PS_DISABLE_CODEC_ADC 1      // 1 = power down shield ADC/I2S-out (INMP441 on pin 8); 0 = leave codec untouched
#ifndef PS_LOOKAHEAD_MS
#define PS_LOOKAHEAD_MS 2.0f        // algorithmic look-ahead
#endif
#ifndef PS_RAMP_MS
#define PS_RAMP_MS 1.0f             // length of the Hann gain-drop kernel
#endif
#ifndef PS_RMSWIN_MS
#define PS_RMSWIN_MS 1.0f           // RMS-ceiling window (must satisfy D >= KK + WR - 2)
#endif
#ifndef PS_SAFE_RMS
#define PS_SAFE_RMS 0.15f           // max 5 ms output RMS (PLACEHOLDER: calibrate)
#endif
#ifndef PS_SAFE_PEAK
#define PS_SAFE_PEAK 0.5f           // max |sample| (PLACEHOLDER: calibrate)
#endif
#ifndef PS_START_GAIN
#define PS_START_GAIN 0.1f
#endif
#ifndef PS_CAP_DB
#define PS_CAP_DB 1.5f
#endif
#ifndef PS_ABS_CAP
#define PS_ABS_CAP 0.06f            // event-time RMS cap (PLACEHOLDER: calibrate)
#endif
#ifndef PS_GAIN_FLOOR
#define PS_GAIN_FLOOR 0.01f         // -40 dB event attenuation floor
#endif
#ifndef PS_HOWL
#define PS_HOWL 1                   // 1 = slow-growth/tonal detector on
#endif
#ifndef PS_DC_HZ
#define PS_DC_HZ 0.2f               // DC-removal corner; low so "unity" audio is not coloured (host-tested 0.1-0.5 Hz)
#endif
#ifndef PS_ONSET_LT
#define PS_ONSET_LT 6.0f            // onset must also exceed this multiple of the long-term (50 ms) RMS before the rise (3 false-triggered on noise-like controls)
#endif
#ifndef PS_TELEMETRY
#define PS_TELEMETRY 1
#endif

#include <math.h>
#include <stdint.h>
#include <string.h>
#ifndef PS_HOST
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#endif
#ifndef PS_SR
#ifdef PS_HOST
#define PS_SR 44100.0f
#else
#define PS_SR AUDIO_SAMPLE_RATE_EXACT
#endif
#endif

namespace ps8 {
constexpr int msToN(float ms) { return (int)(ms * PS_SR / 1000.0f + 0.001f); }
constexpr int kD = msToN(PS_LOOKAHEAD_MS), kKK = msToN(PS_RAMP_MS), kWR = msToN(PS_RMSWIN_MS);
constexpr int kWE = msToN(1.0f), kW5 = msToN(5.0f), kWLT = msToN(50.0f), kPKN = msToN(1.0f), kEndN = msToN(2.0f);
constexpr int kNT = 5;
constexpr int kSH[kNT] = {msToN(5), msToN(10), msToN(20), msToN(40), msToN(80)};
constexpr int kHN = kSH[kNT - 1] + 2, kWarmupN = kSH[kNT - 1] + kWLT;
constexpr int kDecim = msToN(1.0f), kSedN = 256, kSedLag = 160, kSedStab = 20;   // 1 ms decimated history (howl path)

static_assert(kKK >= 2 && kWR >= 1 && kWE >= 1 && kPKN >= 1 && kDecim >= 1, "windows too short for this sample rate");
static_assert(kD >= kKK + kWR - 2, "look-ahead too short for the RMS theorem: need D >= KK + WR - 2");
static_assert(kSedLag + 2 < kSedN && kSedStab < kSedN, "howl history too small");
static_assert(PS_SAFE_PEAK > 0.0f && PS_SAFE_PEAK <= 1.0f, "PS_SAFE_PEAK must be in (0,1]");
static_assert(PS_SAFE_RMS > 0.0f && PS_SAFE_RMS <= PS_SAFE_PEAK, "PS_SAFE_RMS must be in (0,PS_SAFE_PEAK]");
static_assert(PS_ABS_CAP > 0.0f && PS_ABS_CAP <= PS_SAFE_RMS, "PS_ABS_CAP (event cap) must be in (0,PS_SAFE_RMS]");
static_assert(PS_START_GAIN > 0.0f && PS_START_GAIN <= 1.0f, "bad start gain");
static_assert(PS_GAIN_FLOOR > 0.0f && PS_GAIN_FLOOR <= 1.0f, "bad gain floor");
static_assert(PS_DC_HZ > 0.0f, "bad DC corner");

const float kOnsetDb[kNT] = {6.0f, 12.0f, 24.0f, 30.0f, 36.0f};
const float kLoudFloor = 0.02f, kLtRatio = 3.0f, kBaseRef = 0.02f;
const float kSedFloor = 0.03f, kSedRiseDb = 15.0f, kSedStabDb = 6.0f, kSedTonal = 0.84f;
const float kPeakQ = PS_SAFE_PEAK, kRmsQ = PS_SAFE_RMS, kOnsetLt = PS_ONSET_LT;

// Max (kMax=true) or min of the last N pushed values, amortised O(1) (monotonic deque in a ring).
template <int N, bool kMax>
struct SlidingExtreme {
  float val[N];
  uint32_t stamp[N];
  int head, len;
  uint32_t now;
  void clear() { head = 0; len = 0; now = 0; }
  void fill(float v) { clear(); for (int i = 0; i < N; i++) push(v); }
  inline float push(float x) {
    while (len > 0 && (uint32_t)(now - stamp[head]) >= (uint32_t)N) { if (++head == N) head = 0; --len; }
    int tail = head + len; if (tail >= N) tail -= N;
    while (len > 0) {
      int b = (tail == 0) ? N - 1 : tail - 1;
      if (kMax ? (val[b] <= x) : (val[b] >= x)) { tail = b; --len; } else break;
    }
    val[tail] = x; stamp[tail] = now; ++len; ++now;
    return val[head];
  }
};

// ---- state ----
float dly[kD]; int dpos;                         // delayed audio (scaled int16 units), kD samples of delay
uint32_t sqE[kWE], sqW[kWR], sq5[kW5], sqL[kWLT]; int pE, pW, p5, pL;
int64_t sE, sW, s5, sL;
uint16_t ab5[kW5]; int64_t sA5;                  // |x| ring for tonality
SlidingExtreme<kPKN, true> pkMax;
float hdb[kHN], henv[kHN], hlt[kHN]; int hpos; int count;
float sedE[kSedN], sedL[kSedN]; int sedpos, sedcnt, decc; bool sedFlag; float sedBase, sedNeed;
float tg, g0, targetGain, base_, epPeak; bool inEp; int quiet;
SlidingExtreme<kD + 1, false> planMin;          // forward-min of the release-shaped plan over D+1 samples
SlidingExtreme<kKK, false> uMin;                // exact-unity detector for the Hann window
float ubuf[kKK], wk[kKK]; int upos;
float qPrev, rho, rhoMax, acc, capK, dcA, rWr, gain;
uint32_t yE5[kW5]; int py5; int64_t sY5, lim5; int32_t pki;
float dcEst; bool dcPrimed; int dcN;
float gainMin;
uint32_t guardPeak, guardRms, eventSamples, howlSamples, eventCount, faults;

void reset() {
  for (int i = 0; i < kD; i++) dly[i] = 0;
  for (int i = 0; i < kWE; i++) sqE[i] = 0;
  for (int i = 0; i < kWR; i++) sqW[i] = 0;
  for (int i = 0; i < kW5; i++) { sq5[i] = 0; ab5[i] = 0; yE5[i] = 0; }
  for (int i = 0; i < kWLT; i++) sqL[i] = 0;
  for (int i = 0; i < kHN; i++) hdb[i] = henv[i] = hlt[i] = 0;
  for (int i = 0; i < kSedN; i++) sedE[i] = sedL[i] = 0;
  pkMax.fill(0.0f); planMin.fill(PS_START_GAIN); uMin.fill(PS_START_GAIN);
  pE = pW = p5 = pL = hpos = count = dpos = upos = py5 = sedpos = sedcnt = decc = 0;
  sE = sW = s5 = sL = sA5 = sY5 = 0; sedFlag = false; sedBase = kBaseRef; sedNeed = 1.0f;
  float ws = 0;
  for (int i = 0; i < kKK; i++) { wk[i] = 0.5f - 0.5f * cosf(6.2831853f * (i + 1) / (kKK + 1)); ws += wk[i]; }
  for (int i = 0; i < kKK; i++) { wk[i] /= ws; ubuf[i] = PS_START_GAIN; }
  tg = g0 = targetGain = 1; base_ = epPeak = 0; inEp = false; quiet = 0;
  dcEst = 0; dcPrimed = false; dcN = 0;
  rhoMax = (30.0f / 0.22f) / 20.0f * 2.302585f / PS_SR;       // nepers/sample = 30 dB per 220 ms
  acc = rhoMax / (0.020f * PS_SR);                              // reaches max release speed in 20 ms
  qPrev = logf(PS_START_GAIN); rho = 0; gain = PS_START_GAIN; gainMin = 1.0f;
  capK = powf(10.0f, PS_CAP_DB / 20.0f);
  dcA = 1.0f - expf(-6.2831853f * PS_DC_HZ / PS_SR);
  int m = (kW5 + kWR - 1) / kWR;                                // WR-windows needed to cover 5 ms
  rWr = 0.99f * kRmsQ / sqrtf((float)m * kWR / kW5);            // 1% margin: float-vs-rounded-sample slack, so the int guard is a true backstop
  const double fs = 32767.0;
  lim5 = (int64_t)floor((double)kW5 * (kRmsQ * fs) * (kRmsQ * fs));
  pki = (int32_t)floorf(kPeakQ * 32767.0f);
  guardPeak = guardRms = eventSamples = howlSamples = eventCount = faults = 0;
}

inline void faultRecover() {
  ++faults; qPrev = logf(PS_START_GAIN); rho = 0;
  tg = g0 = targetGain = 1.0f; inEp = false; epPeak = 0; quiet = 0;
}

inline int16_t step(int16_t in, float* gain_out = 0) {
  // DC removal: primed on the first sample, running mean while the history is short, then a leaky estimate (corner
  // PS_DC_HZ). Then exact integer quantisation for all window sums.
  float raw = (float)in * (1.0f / 32768.0f);
  float x;
  if (!dcPrimed) { dcEst = raw; dcPrimed = true; dcN = 1; x = 0.0f; }
  else {
    x = raw - dcEst;
    if (dcN < 1000000000) ++dcN;
    float k = 1.0f / (float)dcN; if (k < dcA) k = dcA;
    dcEst += k * x;
  }
  float xs = x * 32768.0f;
  int32_t xi = (int32_t)lrintf(xs);
  if (xi > 32767) xi = 32767;
  if (xi < -32768) xi = -32768;
  uint32_t sq = (uint32_t)((int64_t)xi * xi);
  uint16_t axi = (uint16_t)(xi < 0 ? -xi : xi);
  const float ax = (float)axi * (1.0f / 32768.0f);

  sE += (int64_t)sq - sqE[pE]; sqE[pE] = sq; if (++pE == kWE) pE = 0;
  sW += (int64_t)sq - sqW[pW]; sqW[pW] = sq; if (++pW == kWR) pW = 0;
  s5 += (int64_t)sq - sq5[p5]; sq5[p5] = sq; sA5 += (int64_t)axi - ab5[p5]; ab5[p5] = axi; if (++p5 == kW5) p5 = 0;
  sL += (int64_t)sq - sqL[pL]; sqL[pL] = sq; if (++pL == kWLT) pL = 0;
  const float inv = 1.0f / 32768.0f;
  float env  = sqrtf((float)sE / kWE) * inv;
  float envW = sqrtf((float)sW / kWR) * inv;
  float lt   = sqrtf((float)sL / kWLT) * inv;
  hdb[hpos] = 20.0f * log10f(env + 1e-6f); henv[hpos] = env; hlt[hpos] = lt;
  float level = fmaxf(env, pkMax.push(ax) * 0.70710678f);

  // ---- onset detector (rate of rise, 5 time scales, 1 ms envelope) ----
  bool flag = false; float g_need = 1.0f, base_hit = 1e9f;
  for (int j = 0; j < kNT; j++) {
    if (count < kSH[j] + kWLT) continue;
    int p = hpos - kSH[j]; if (p < 0) p += kHN;
    if (hdb[hpos] - hdb[p] > kOnsetDb[j] && env > kLoudFloor && env > kOnsetLt * hlt[p]) {
      flag = true; g_need = fminf(g_need, fmaxf(PS_GAIN_FLOOR, henv[p] / level)); base_hit = fminf(base_hit, henv[p]);
    }
  }
  hpos++; if (hpos == kHN) hpos = 0; if (count < 1000000000) count++;

#if PS_HOWL
  // ---- slow-growth detector: evaluated once per ms on a decimated history, result latched until the next ms ----
  if (++decc >= kDecim) {
    decc = 0;
    const float e5 = sqrtf((float)s5 / kW5) * inv;
    sedE[sedpos] = e5; sedL[sedpos] = lt;
    int cur = sedpos; sedpos = (sedpos + 1) % kSedN; if (sedcnt < kSedN) sedcnt++;
    sedFlag = false;
    if (sedcnt > kSedLag + 1 && count >= kWarmupN && e5 > kSedFloor) {
      int pl = (cur - kSedLag + kSedN) % kSedN;
      float rise = 20.0f * log10f((e5 + 1e-9f) / (sedE[pl] + 1e-9f));
      float mn = 1e9f, mx = 0;
      for (int i = 0; i < kSedStab; i++) { float v = sedE[(cur - i + kSedN) % kSedN]; mn = fminf(mn, v); mx = fmaxf(mx, v); }
      float stab = 20.0f * log10f((mx + 1e-9f) / (mn + 1e-9f));
      float tonal = ((float)sA5 / kW5) / (e5 * 32768.0f + 1e-9f);
      if (rise > kSedRiseDb && e5 > kLtRatio * sedL[pl] && stab < kSedStabDb && tonal > kSedTonal) {
        sedFlag = true; sedBase = sedE[pl];
        sedNeed = fminf(1.0f, fmaxf(PS_GAIN_FLOOR, sedE[pl] / level));
      }
    }
  }
  if (sedFlag) { flag = true; base_hit = fminf(base_hit, sedBase); g_need = fminf(g_need, sedNeed); howlSamples++; }
#endif
  if (flag && base_hit > 1e8f) base_hit = kBaseRef;

  // ---- event tracking + planned gain (policy as v7) ----
  if (flag) {
    if (!inEp) { g0 = 1.0f; base_ = base_hit; epPeak = 0; eventCount++; }
    inEp = true; quiet = 0; base_ = fminf(base_, base_hit);
  } else if (inEp) {
    quiet = (env < 0.3f * epPeak) ? quiet + 1 : 0;
    if (quiet >= kEndN) { inEp = false; epPeak = 0; }
  }
  if (inEp) {
    eventSamples++;
    epPeak = fmaxf(epPeak, env);
    float cap = fminf(fmaxf(base_, kBaseRef) * capK, PS_ABS_CAP);
    targetGain = fminf(1.0f, fmaxf(PS_GAIN_FLOOR, cap / fmaxf(epPeak, 1e-9f)));
    if (flag) { g0 = fminf(g0, g_need); tg = fminf(fminf(tg, g0), targetGain); }
    tg = fminf(targetGain, tg * 1.00036f);   // loudness may creep toward the cap
  } else tg = 1.0f;

  // ---- provable bounds: peak and WR-window RMS (never floored: r >= min(floor, peak, rms) > 0) ----
  float r = tg;
  if (ax > kPeakQ) r = fminf(r, kPeakQ / ax);
  if (envW > rWr) r = fminf(r, rWr / envW);
  if (count < kWarmupN) r = fminf(r, PS_START_GAIN);

  // ---- release shaping in log domain (acceleration-limited S-curve, braking landing at 0 dB) ----
  float lr = logf(r), rr = rho + acc; if (rr > rhoMax) rr = rhoMax;
  float room = -qPrev; if (room < 0) room = 0;
  float brake = sqrtf(2.0f * acc * room); if (rr > brake) rr = brake;
  float Z = qPrev + rr; if (Z > 0) Z = 0;
  float Q;
  if (lr < Z) { Q = lr; rho = 0; } else { Q = Z; rho = rr; }
  if (!(Q <= 0.0f && Q > -20.0f)) { faultRecover(); Q = qPrev; }   // also catches NaN
  qPrev = Q;

  // ---- forward-min over D+1 plan samples, then causal Hann kernel (exactly 1.0 when the window is all 1.0) ----
  float u = planMin.push(expf(Q));
  ubuf[upos] = u; if (++upos == kKK) upos = 0;
  float g;
  if (uMin.push(u) >= 1.0f) g = 1.0f;
  else {
    g = 0; int idx = upos;
    for (int i = 0; i < kKK; i++) { idx = (idx == 0) ? kKK - 1 : idx - 1; g += wk[i] * ubuf[idx]; }
    if (g > 1.0f) g = 1.0f;
    if (!(g >= 0.0f)) { g = 0.0f; ++faults; }
  }
  gain = g; if (g < gainMin) gainMin = g;

  // ---- delayed audio * gain, then exact int16 guards ----
  float xd = dly[dpos]; dly[dpos] = (float)xi; if (++dpos == kD) dpos = 0;
  int32_t yi = (int32_t)(xd * g);                 // truncation toward zero never increases magnitude
  if (yi > pki) { yi = pki; guardPeak++; } else if (yi < -pki) { yi = -pki; guardPeak++; }
  uint32_t old = yE5[py5];
  int64_t used = sY5 - (int64_t)old;
  int64_t sqy = (int64_t)yi * yi;
  if (used + sqy > lim5) {
    int64_t room2 = lim5 - used; if (room2 < 0) room2 = 0;
    int32_t m = (int32_t)sqrt((double)room2);
    while ((int64_t)m * m > room2) m--;
    yi = yi < 0 ? -m : m; sqy = (int64_t)yi * yi; guardRms++;
  }
  sY5 = used + sqy; yE5[py5] = (uint32_t)sqy; if (++py5 == kW5) py5 = 0;
  if (gain_out) *gain_out = g;
  return (int16_t)yi;
}
}  // namespace ps8

#ifdef PS_HOST
extern "C" void ps_reset() { ps8::reset(); }
extern "C" void ps_process(const int16_t* in, int16_t* out, float* g, int n) {
  for (int i = 0; i < n; i++) { float gg; out[i] = ps8::step(in[i], &gg); g[i] = gg; }
}
extern "C" int ps_delay() { return ps8::kD; }
extern "C" unsigned ps_guard_peak() { return ps8::guardPeak; }
extern "C" unsigned ps_guard_rms() { return ps8::guardRms; }
extern "C" unsigned ps_event_samples() { return ps8::eventSamples; }
extern "C" unsigned ps_howl_samples() { return ps8::howlSamples; }
extern "C" unsigned ps_event_count() { return ps8::eventCount; }
extern "C" unsigned ps_faults() { return ps8::faults; }
extern "C" float ps_rho_max() { return ps8::rhoMax; }
#else
// ---------------- Teensy 4.0 audio plumbing ----------------
static inline uint32_t psCycles() {
#ifdef ARM_DWT_CYCCNT
  return ARM_DWT_CYCCNT;
#else
  return micros();
#endif
}
#ifdef ARM_DWT_CYCCNT
#ifdef F_CPU_ACTUAL
static const float kCyclesPerUs = (float)F_CPU_ACTUAL / 1.0e6f;
#else
static const float kCyclesPerUs = (float)F_CPU / 1.0e6f;
#endif
#else
static const float kCyclesPerUs = 1.0f;
#endif

class AudioEffectPulseShield : public AudioStream {
public:
  AudioEffectPulseShield() : AudioStream(1, inQ) {}
  volatile bool ready = false;                  // set by setup() once reset() and codec init are done
  volatile uint32_t early = 0, missing = 0, dropped = 0, blocks = 0, cycMaxEver = 0, cycMaxWin = 0;
  volatile uint64_t cycSum = 0;
  virtual void update(void) {
    audio_block_t* b = receiveWritable(0);
    if (!ready) {                               // setup() not finished: never touch DSP state, play silence
      if (b) release(b);
      early++;
      return;
    }
    if (!b) {                                   // keep the DSP time-aligned: process one block of silence
      missing++;
      b = allocate();
      if (!b) { dropped++; return; }            // no audio memory at all: this block is lost (counted)
      memset(b->data, 0, sizeof(b->data));
    }
    const uint32_t c0 = psCycles();
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) b->data[i] = ps8::step(b->data[i]);
    const uint32_t dt = psCycles() - c0;
    cycSum += dt; blocks++;
    if (dt > cycMaxEver) cycMaxEver = dt;
    if (dt > cycMaxWin) cycMaxWin = dt;
    transmit(b); release(b);
  }
private:
  audio_block_t* inQ[1];
};

AudioInputI2S i2s_mic;
AudioEffectPulseShield ps;
AudioOutputI2S i2s_out;
AudioControlSGTL5000 sgtl5000_1;
AudioConnection patchCord1(i2s_mic, 0, ps, 0);
AudioConnection patchCord2(ps, 0, i2s_out, 0);
AudioConnection patchCord3(ps, 0, i2s_out, 1);

static bool sgtl_write(uint16_t reg, uint16_t value) {
  Wire.beginTransmission(0x0A);
  Wire.write((uint8_t)(reg >> 8)); Wire.write((uint8_t)(reg & 0xFF));
  Wire.write((uint8_t)(value >> 8)); Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}
static uint8_t sgtl_probe() { Wire.beginTransmission(0x0A); return Wire.endTransmission(); }
static bool sgtl_read(uint16_t reg, uint16_t* value) {
  Wire.beginTransmission(0x0A);
  Wire.write((uint8_t)(reg >> 8)); Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)0x0A, (uint8_t)2) != 2) return false;
  uint16_t v = (uint16_t)Wire.read() << 8; v |= (uint16_t)Wire.read();
  *value = v; return true;
}
// Write, read back, retry up to 3 times. Returns true only if the read-back equals the value written.
static bool sgtl_write_verified(uint16_t reg, uint16_t value) {
  for (int t = 0; t < 3; t++) {
    uint16_t rb = 0;
    if (sgtl_write(reg, value) && sgtl_read(reg, &rb) && rb == value) return true;
    delay(5);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  uint32_t tWait = millis();
  while (!Serial && millis() - tWait < 2000) {}
  ps8::reset();                                  // before the audio interrupt can ever call step()
  AudioMemory(20);
  bool codec_ok = false;
  for (int a = 1; a <= 5 && !codec_ok; a++) {
    codec_ok = sgtl5000_1.enable();
    if (!codec_ok) { Serial.print("SGTL5000 enable attempt "); Serial.print(a); Serial.print(" failed, probe "); Serial.println(sgtl_probe()); delay(150); }
  }
  sgtl5000_1.volume(PS_HP_VOLUME);
#if PS_DISABLE_CODEC_ADC
  bool dig_ok = sgtl_write_verified(0x0002, 0x0021);   // CHIP_DIG_POWER
  bool ana_ok = sgtl_write_verified(0x0030, 0x40FD);   // CHIP_ANA_POWER
  sgtl5000_1.volume(PS_HP_VOLUME);
#else
  bool dig_ok = true, ana_ok = true;
#endif
  ps.ready = true;                               // audio path goes live only now
  Serial.println("PulseShield v8 audio-shield build");
  Serial.print("SGTL5000 enable: "); Serial.println(codec_ok ? "OK" : "FAIL");
  Serial.print("SGTL dig/ana: "); Serial.print(dig_ok ? "OK" : "FAIL"); Serial.print("/"); Serial.println(ana_ok ? "OK" : "FAIL");
  Serial.print("PS_SR "); Serial.print(PS_SR, 2); Serial.print(" block "); Serial.print(AUDIO_BLOCK_SAMPLES);
  Serial.print(" look-ahead samples "); Serial.print(ps8::kD);
  Serial.print(" = "); Serial.print(1000.0f * ps8::kD / PS_SR, 2); Serial.println(" ms (end-to-end latency must be measured)");
}

void loop() {
#if PS_TELEMETRY
  static uint32_t tTel = 0;
  if (millis() - tTel > 2000 && Serial.availableForWrite() > 280) {
    tTel = millis();
    noInterrupts();
    uint64_t sum = ps.cycSum; uint32_t blk = ps.blocks, mxE = ps.cycMaxEver, mxW = ps.cycMaxWin;
    ps.cycMaxWin = 0;
    interrupts();
    const float budgetUs = 1.0e6f * AUDIO_BLOCK_SAMPLES / PS_SR;
    const float avgUs = blk ? (float)((double)sum / blk) / kCyclesPerUs : 0.0f;
    Serial.print("DSP us/block avg "); Serial.print(avgUs, 1);
    Serial.print(" max "); Serial.print(mxE / kCyclesPerUs, 1);
    Serial.print(" (last 2s "); Serial.print(mxW / kCyclesPerUs, 1);
    Serial.print(") budget "); Serial.print(budgetUs, 0);
    Serial.print(" = "); Serial.print(100.0f * (mxE / kCyclesPerUs) / budgetUs, 1); Serial.print("% of block");
    Serial.print(" | AudioProcessorUsageMax "); Serial.print(AudioProcessorUsageMax());
    Serial.print("% | mem max "); Serial.print(AudioMemoryUsageMax());
    Serial.print(" | early "); Serial.print(ps.early);
    Serial.print(" | missing-in "); Serial.print(ps.missing);
    Serial.print(" | dropped "); Serial.print(ps.dropped);
    Serial.print(" | faults "); Serial.print(ps8::faults);
    Serial.print(" | guard peak/rms "); Serial.print(ps8::guardPeak); Serial.print("/"); Serial.print(ps8::guardRms);
    Serial.print(" | events "); Serial.print(ps8::eventCount);
    Serial.print(" | event/howl samples "); Serial.print(ps8::eventSamples); Serial.print("/"); Serial.print(ps8::howlSamples);
    Serial.print(" | min gain dB "); Serial.println(20.0f * log10f(ps8::gainMin + 1e-9f), 1);
  }
#endif
}
#endif
