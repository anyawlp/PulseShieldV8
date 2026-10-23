#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>

extern "C" {
  void ps_reset();
  void ps_process(const int16_t* in, int16_t* out, float* g, int n);
  int ps_delay();
}

struct WavHeader {
    char riff[4];
    uint32_t overall_size;
    char wave[4];
    char fmt_chunk_marker[4];
    uint32_t length_of_fmt;
    uint16_t format_type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byterate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_chunk_header[4];
    uint32_t data_size;
};

bool process_wav_file(const std::string& in_path, const std::string& out_path) {
    std::ifstream in(in_path, std::ios::binary);
    if (!in.is_open()) return false;

    WavHeader hdr;
    in.read(reinterpret_cast<char*>(&hdr), sizeof(WavHeader));

    int num_samples = hdr.data_size / sizeof(int16_t);
    std::vector<int16_t> in_data(num_samples);
    in.read(reinterpret_cast<char*>(in_data.data()), hdr.data_size);
    in.close();

    std::vector<int16_t> out_data(num_samples);
    std::vector<float> gains(num_samples);

    ps_reset();
    ps_process(in_data.data(), out_data.data(), gains.data(), num_samples);

    std::ofstream out(out_path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(WavHeader));
    out.write(reinterpret_cast<const char*>(out_data.data()), hdr.data_size);
    out.close();

    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input.wav> <output.wav>\n";
        return 1;
    }
    if (process_wav_file(argv[1], argv[2])) {
        std::cout << "Successfully processed " << argv[1] << " -> " << argv[2] << " through PulseShield v8 DSP engine.\n";
        return 0;
    } else {
        std::cerr << "Failed to process audio file.\n";
        return 1;
    }
}
