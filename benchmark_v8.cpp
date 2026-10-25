#include <iostream>
#include <vector>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <numeric>
#include <algorithm>

// Host declarations exported by PulseShield_v8_audioshield.ino
extern "C" {
  void ps_reset();
  void ps_process(const int16_t* in, int16_t* out, float* g, int n);
  int ps_delay();
  unsigned ps_guard_peak();
  unsigned ps_guard_rms();
  unsigned ps_event_samples();
  unsigned ps_howl_samples();
  unsigned ps_event_count();
  unsigned ps_faults();
  float ps_rho_max();
}

struct TestResult {
    std::string name;
    float peak_reduction_db;
    float max_output_rms_dbfs;
    float recovery_ms;
    unsigned guard_peak;
    unsigned guard_rms;
    unsigned faults;
    bool passed;
};

// Generates synthetic signals matching the paper's battery specification
std::vector<int16_t> generate_stimulus(
    const std::string& event_type, 
    float background_dbfs, 
    int sample_rate = 44100, 
    float duration_s = 2.5f
) {
    int total_samples = static_cast<int>(sample_rate * duration_s);
    std::vector<int16_t> signal(total_samples, 0);

    float bg_amp = std::pow(10.0f, background_dbfs / 20.0f);
    int event_start = static_cast<int>(1.0f * sample_rate); // starts at 1.0 s
    int event_dur = (event_type == "siren_triage") ? static_cast<int>(0.8f * sample_rate) : static_cast<int>(0.05f * sample_rate);

    // PRNG seed state
    uint32_t seed = 1337;
    auto rnd = [&]() -> float {
        seed = seed * 1664525 + 1013904223;
        return (static_cast<float>(seed & 0xFFFF) / 32768.0f) - 1.0f;
    };

    for (int i = 0; i < total_samples; i++) {
        // Base background (pink/speech-like filtered noise)
        float sample = bg_amp * rnd() * 0.5f;

        // Add event
        if (i >= event_start && i < event_start + event_dur) {
            float t = static_cast<float>(i - event_start) / sample_rate;
            float event_val = 0.0f;
            float peak_target = 0.90f; // 0.9 FS peak as in paper

            if (event_type == "clap") {
                // Exponential decay impulse
                float env = std::exp(-t * 200.0f);
                event_val = peak_target * env * rnd();
            } else if (event_type == "alarm" || event_type == "siren_triage") {
                // 3 kHz tone burst (transient burst or sustained siren)
                event_val = peak_target * std::sin(2.0f * M_PI * 3000.0f * t);
            } else if (event_type == "door_slam") {
                // Low frequency resonant burst (80 Hz + noise)
                float env = std::exp(-t * 40.0f);
                event_val = peak_target * env * (0.7f * std::sin(2.0f * M_PI * 80.0f * t) + 0.3f * rnd());
            } else if (event_type == "shout") {
                // 600 Hz harmonic with vibrato
                float freq = 600.0f + 30.0f * std::sin(2.0f * M_PI * 6.0f * t);
                event_val = peak_target * std::sin(2.0f * M_PI * freq * t);
            } else if (event_type == "howl") {
                // Slow growing tonal feedback
                float ramp = std::min(1.0f, t / 0.04f);
                event_val = peak_target * ramp * std::sin(2.0f * M_PI * 2200.0f * t);
            } else if (event_type == "firecracker") {
                // Instantaneous square-ish impulse + resonant tail
                float env = std::exp(-t * 350.0f);
                event_val = peak_target * env * (rnd() > 0 ? 1.0f : -1.0f);
            }

            sample += event_val;
        }

        // Clamp to 16-bit PCM
        sample = std::max(-1.0f, std::min(1.0f, sample));
        signal[i] = static_cast<int16_t>(sample * 32767.0f);
    }

    return signal;
}

TestResult evaluate_event(const std::string& name, const std::string& type, float bg_dbfs) {
    auto input = generate_stimulus(type, bg_dbfs);
    int n = input.size();
    std::vector<int16_t> output(n, 0);
    std::vector<float> gains(n, 1.0f);

    ps_reset();
    ps_process(input.data(), output.data(), gains.data(), n);

    // Compute input peak & output peak in event window [1.0s, 1.15s]
    int start = 44100 * 1.0;
    int end = 44100 * 1.15;
    int d = ps_delay();

    float in_peak = 0.0f;
    float out_peak = 0.0f;
    float out_rms_sum = 0.0f;

    for (int i = start; i < end; i++) {
        float in_abs = std::abs(static_cast<float>(input[i]) / 32768.0f);
        in_peak = std::max(in_peak, in_abs);

        int out_idx = i + d;
        if (out_idx < n) {
            float out_abs = std::abs(static_cast<float>(output[out_idx]) / 32768.0f);
            out_peak = std::max(out_peak, out_abs);
            out_rms_sum += out_abs * out_abs;
        }
    }

    float peak_red_db = 20.0f * std::log10((in_peak + 1e-6f) / (out_peak + 1e-6f));
    float out_rms = std::sqrt(out_rms_sum / (end - start));
    float out_rms_dbfs = 20.0f * std::log10(out_rms + 1e-6f);

    // Measure recovery time: time from event end until gain >= 0.9 (-0.9 dB)
    float recovery_ms = 0.0f;
    for (int i = end; i < n; i++) {
        if (gains[i] >= 0.90f) {
            recovery_ms = (i - end) * 1000.0f / 44100.0f;
            break;
        }
    }

    TestResult res;
    res.name = name;
    res.peak_reduction_db = peak_red_db;
    res.max_output_rms_dbfs = out_rms_dbfs;
    res.recovery_ms = recovery_ms;
    res.guard_peak = ps_guard_peak();
    res.guard_rms = ps_guard_rms();
    res.faults = ps_faults();
    res.passed = (peak_red_db >= 10.0f) && (res.faults == 0);

    return res;
}

int main() {
    std::cout << "========================================================================\n";
    std::cout << "       PULSESHIELD V8 ELITE HOST BENCHMARK & REGRESSION AUDIT           \n";
    std::cout << "========================================================================\n";
    std::cout << "Compiled with -DPS_HOST against Teensy 4.0 DSP Core\n";
    std::cout << "Look-ahead delay: " << ps_delay() << " samples (" << (ps_delay() * 1000.0f / 44100.0f) << " ms)\n\n";

    std::vector<std::tuple<std::string, std::string, float>> cases = {
        {"Clap (Quiet)", "clap", -50.0f},
        {"Clap (Speech)", "clap", -40.0f},
        {"Clap (Loud)", "clap", -30.0f},
        {"Alarm 3k (Quiet)", "alarm", -50.0f},
        {"Alarm 3k (Speech)", "alarm", -40.0f},
        {"Alarm 3k (Loud)", "alarm", -30.0f},
        {"Door Slam (Quiet)", "door_slam", -50.0f},
        {"Door Slam (Speech)", "door_slam", -40.0f},
        {"Door Slam (Loud)", "door_slam", -30.0f},
        {"Howl/Tonal (Quiet)", "howl", -50.0f},
        {"Howl/Tonal (Speech)", "howl", -40.0f},
        {"Howl/Tonal (Loud)", "howl", -30.0f},
        {"Shout 600Hz (Speech)", "shout", -40.0f},
        {"Firecracker (Quiet)", "firecracker", -50.0f},
        {"Firecracker (Loud)", "firecracker", -30.0f},
        {"Siren Triage (70dB Evac)", "siren_triage", -40.0f}
    };

    std::cout << std::left << std::setw(24) << "Test Case"
              << std::setw(16) << "Peak Reduction"
              << std::setw(16) << "Out RMS (dBFS)"
              << std::setw(15) << "Recovery (ms)"
              << std::setw(10) << "Faults"
              << std::setw(8) << "Status" << "\n";
    std::cout << std::string(89, '-') << "\n";

    int passed_count = 0;
    for (const auto& c : cases) {
        auto res = evaluate_event(std::get<0>(c), std::get<1>(c), std::get<2>(c));
        std::cout << std::left << std::setw(24) << res.name
                  << std::setw(14) << (std::to_string(res.peak_reduction_db).substr(0, 5) + " dB")
                  << std::setw(16) << (std::to_string(res.max_output_rms_dbfs).substr(0, 6) + " dBFS")
                  << std::setw(15) << (std::to_string(res.recovery_ms).substr(0, 5) + " ms")
                  << std::setw(10) << res.faults
                  << (res.passed ? "[PASS]" : "[FAIL]") << "\n";
        if (res.passed) passed_count++;
    }

    std::cout << std::string(89, '-') << "\n";
    std::cout << "SUMMARY: " << passed_count << " / " << cases.size() << " tests passed with >= 10 dB peak reduction.\n";
    std::cout << "Hardware Safety Guards: Peak " << ps_guard_peak() << ", RMS " << ps_guard_rms() << ", Faults " << ps_faults() << "\n";
    std::cout << "========================================================================\n";

    return (passed_count == cases.size()) ? 0 : 1;
}
