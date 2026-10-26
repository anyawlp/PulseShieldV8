#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>

extern "C" {
  void ps_reset();
  void ps_process(const int16_t* in, int16_t* out, float* g, int n);
  int ps_delay();
}

struct WavFormat {
    uint16_t format_tag = 0;       // 1 = PCM, 3 = IEEE Float, 0xFFFE = Extensible
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t byte_rate = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    uint16_t valid_bits = 0;
    uint32_t channel_mask = 0;
    uint16_t sub_format_tag = 0;
};

bool process_wav_file(const std::string& in_path, const std::string& out_path) {
    std::ifstream in(in_path, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "Cannot open input file: " << in_path << "\n";
        return false;
    }

    in.seekg(0, std::ios::end);
    uint64_t file_size = in.tellg();
    in.seekg(0, std::ios::beg);

    if (file_size < 12) {
        std::cerr << "Invalid WAV: file too small (" << file_size << " bytes)\n";
        return false;
    }

    char riff_id[4];
    uint32_t riff_size = 0;
    char wave_id[4];

    in.read(riff_id, 4);
    in.read(reinterpret_cast<char*>(&riff_size), 4);
    in.read(wave_id, 4);

    if (std::memcmp(riff_id, "RIFF", 4) != 0 || std::memcmp(wave_id, "WAVE", 4) != 0) {
        std::cerr << "Invalid WAV: missing RIFF/WAVE header\n";
        return false;
    }

    bool has_fmt = false;
    bool has_data = false;
    WavFormat fmt;
    std::vector<uint8_t> raw_data;
    uint32_t data_size = 0;

    while (in.good() && in.tellg() < (std::streampos)file_size) {
        char chunk_id[4];
        uint32_t chunk_size = 0;
        if (!in.read(chunk_id, 4)) break;
        if (!in.read(reinterpret_cast<char*>(&chunk_size), 4)) break;

        std::streampos chunk_data_pos = in.tellg();

        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            has_fmt = true;
            if (chunk_size >= 16) {
                in.read(reinterpret_cast<char*>(&fmt.format_tag), 2);
                in.read(reinterpret_cast<char*>(&fmt.channels), 2);
                in.read(reinterpret_cast<char*>(&fmt.sample_rate), 4);
                in.read(reinterpret_cast<char*>(&fmt.byte_rate), 4);
                in.read(reinterpret_cast<char*>(&fmt.block_align), 2);
                in.read(reinterpret_cast<char*>(&fmt.bits_per_sample), 2);

                if (fmt.format_tag == 0xFFFE && chunk_size >= 40) {
                    uint16_t cb_size = 0;
                    in.read(reinterpret_cast<char*>(&cb_size), 2);
                    in.read(reinterpret_cast<char*>(&fmt.valid_bits), 2);
                    in.read(reinterpret_cast<char*>(&fmt.channel_mask), 4);
                    in.read(reinterpret_cast<char*>(&fmt.sub_format_tag), 2);
                } else {
                    fmt.sub_format_tag = fmt.format_tag;
                }
            }
            std::streampos next_pos = chunk_data_pos + (std::streamoff)chunk_size;
            if (chunk_size & 1) next_pos += 1;
            in.seekg(next_pos);
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            has_data = true;
            uint64_t available = (file_size > (uint64_t)chunk_data_pos) ? (file_size - (uint64_t)chunk_data_pos) : 0;
            data_size = chunk_size;
            if ((uint64_t)data_size > available || data_size == 0xFFFFFFFF) {
                data_size = (uint32_t)available;
            }
            if (data_size > 0) {
                raw_data.resize(data_size);
                in.read(reinterpret_cast<char*>(raw_data.data()), data_size);
            }
            break; // Finished reading audio data
        } else {
            // Skip other chunks (LIST, INFO, JUNK, etc.)
            std::streampos next_pos = chunk_data_pos + (std::streamoff)chunk_size;
            if (chunk_size & 1) next_pos += 1;
            in.seekg(next_pos);
        }
    }
    in.close();

    if (!has_fmt) {
        std::cerr << "Invalid WAV: missing 'fmt ' chunk\n";
        return false;
    }
    if (!has_data) {
        std::cerr << "Invalid WAV: missing 'data' chunk\n";
        return false;
    }
    if (fmt.channels == 0 || fmt.sample_rate == 0) {
        std::cerr << "Invalid WAV: channels or sample_rate is 0\n";
        return false;
    }

    uint16_t eff_fmt = (fmt.format_tag == 0xFFFE) ? fmt.sub_format_tag : fmt.format_tag;
    int bytes_per_sample = fmt.bits_per_sample / 8;
    if (bytes_per_sample <= 0) bytes_per_sample = 2;
    int bytes_per_frame = bytes_per_sample * fmt.channels;
    if (bytes_per_frame <= 0) bytes_per_frame = 2;

    size_t num_frames = raw_data.size() / bytes_per_frame;
    if (num_frames == 0) {
        std::cerr << "Warning: 0 audio frames found in " << in_path << "\n";
        // Write empty WAV output
        std::ofstream out(out_path, std::ios::binary);
        if (!out.is_open()) return false;
        char hdr[44] = {0};
        std::memcpy(hdr, "RIFF", 4);
        uint32_t osz = 36;
        std::memcpy(hdr + 4, &osz, 4);
        std::memcpy(hdr + 8, "WAVEfmt ", 8);
        uint32_t fsz = 16;
        std::memcpy(hdr + 16, &fsz, 4);
        uint16_t tag = 1;
        std::memcpy(hdr + 20, &tag, 2);
        std::memcpy(hdr + 22, &fmt.channels, 2);
        std::memcpy(hdr + 24, &fmt.sample_rate, 4);
        uint32_t br = fmt.sample_rate * fmt.channels * 2;
        std::memcpy(hdr + 28, &br, 4);
        uint16_t ba = fmt.channels * 2;
        std::memcpy(hdr + 32, &ba, 2);
        uint16_t bits = 16;
        std::memcpy(hdr + 34, &bits, 2);
        std::memcpy(hdr + 36, "data", 4);
        uint32_t dsz = 0;
        std::memcpy(hdr + 40, &dsz, 4);
        out.write(hdr, 44);
        out.close();
        return true;
    }

    // Convert input audio to 16-bit PCM per channel
    std::vector<std::vector<int16_t>> in_channels(fmt.channels, std::vector<int16_t>(num_frames));
    std::vector<std::vector<int16_t>> out_channels(fmt.channels, std::vector<int16_t>(num_frames));

    const uint8_t* p = raw_data.data();
    for (size_t i = 0; i < num_frames; i++) {
        for (int ch = 0; ch < fmt.channels; ch++) {
            int16_t s = 0;
            if (eff_fmt == 1) { // Integer PCM
                if (fmt.bits_per_sample == 8) {
                    uint8_t u8 = *p++;
                    s = (int16_t)(((int)u8 - 128) << 8);
                } else if (fmt.bits_per_sample == 16) {
                    int16_t v16 = (int16_t)(p[0] | (p[1] << 8));
                    p += 2;
                    s = v16;
                } else if (fmt.bits_per_sample == 24) {
                    int32_t v24 = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
                    if (v24 & 0x800000) v24 |= 0xFF000000;
                    p += 3;
                    s = (int16_t)(v24 >> 8);
                } else if (fmt.bits_per_sample == 32) {
                    int32_t v32 = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
                    p += 4;
                    s = (int16_t)(v32 >> 16);
                } else {
                    p += bytes_per_sample;
                }
            } else if (eff_fmt == 3) { // IEEE Float
                if (fmt.bits_per_sample == 32) {
                    float fval;
                    std::memcpy(&fval, p, 4);
                    p += 4;
                    fval = std::max(-1.0f, std::min(1.0f, fval));
                    s = (int16_t)std::lrint(fval * 32767.0f);
                } else {
                    p += bytes_per_sample;
                }
            } else {
                p += bytes_per_sample;
            }
            in_channels[ch][i] = s;
        }
    }

    // Process each channel independently with PulseShield DSP engine
    std::vector<float> gains(num_frames);
    for (int ch = 0; ch < fmt.channels; ch++) {
        ps_reset();
        ps_process(in_channels[ch].data(), out_channels[ch].data(), gains.data(), (int)num_frames);
    }

    // Write standard 16-bit PCM output WAV preserving channel count and sample rate
    std::ofstream out(out_path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "Cannot open output file: " << out_path << "\n";
        return false;
    }

    uint32_t out_data_bytes = (uint32_t)(num_frames * fmt.channels * sizeof(int16_t));
    uint32_t overall_size = 36 + out_data_bytes;

    char header[44];
    std::memcpy(header, "RIFF", 4);
    std::memcpy(header + 4, &overall_size, 4);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    uint32_t fmt_len = 16;
    std::memcpy(header + 16, &fmt_len, 4);
    uint16_t out_tag = 1; // PCM
    std::memcpy(header + 20, &out_tag, 2);
    std::memcpy(header + 22, &fmt.channels, 2);
    std::memcpy(header + 24, &fmt.sample_rate, 4);
    uint32_t out_byterate = fmt.sample_rate * fmt.channels * sizeof(int16_t);
    std::memcpy(header + 28, &out_byterate, 4);
    uint16_t out_align = fmt.channels * sizeof(int16_t);
    std::memcpy(header + 32, &out_align, 2);
    uint16_t out_bits = 16;
    std::memcpy(header + 34, &out_bits, 2);
    std::memcpy(header + 36, "data", 4);
    std::memcpy(header + 40, &out_data_bytes, 4);

    out.write(header, 44);

    std::vector<int16_t> interleaved_out(num_frames * fmt.channels);
    for (size_t i = 0; i < num_frames; i++) {
        for (int ch = 0; ch < fmt.channels; ch++) {
            interleaved_out[i * fmt.channels + ch] = out_channels[ch][i];
        }
    }
    out.write(reinterpret_cast<const char*>(interleaved_out.data()), out_data_bytes);
    out.close();

    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input.wav> <output.wav>\n";
        return 1;
    }
    if (process_wav_file(argv[1], argv[2])) {
        std::cout << "Successfully processed " << argv[1] << " -> " << argv[2] << " through PulseShield v8-Elite DSP engine.\n";
        return 0;
    } else {
        std::cerr << "Failed to process audio file.\n";
        return 1;
    }
}
