#include "assets/sound_file.hpp"
#include "io/storage.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F>
void rejects(F&& f, const std::string& message) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Accepted: " + message);
}
void put32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        s += static_cast<char>((v >> (8 * i)) & 0xff);
}
void put16(std::string& s, std::uint16_t v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>(v >> 8);
}
// A WAV file of `frames` frames, every sample the bytes of `sample`, with optional extra chunks
// before the data.
std::string wav(std::uint16_t format, std::uint16_t bits, std::uint16_t channels, std::uint32_t rate,
                std::uint32_t frames, const std::string& sample, const std::string& extra = {},
                bool extensible = false) {
    std::string fmt;
    put16(fmt, extensible ? 0xFFFE : format);
    put16(fmt, channels);
    put32(fmt, rate);
    put32(fmt, rate * channels * bits / 8u);
    put16(fmt, static_cast<std::uint16_t>(channels * bits / 8u));
    put16(fmt, bits);
    if (extensible) {
        put16(fmt, 22);
        put16(fmt, bits);
        put32(fmt, 0);
        put16(fmt, format);
        fmt += std::string(14, '\0');
    }
    std::string data;
    for (std::uint32_t i = 0; i < frames * channels; ++i)
        data += sample;
    std::string body = "WAVE";
    body += "fmt ";
    put32(body, static_cast<std::uint32_t>(fmt.size()));
    body += fmt + extra + "data";
    put32(body, static_cast<std::uint32_t>(data.size()));
    body += data;
    std::string file = "RIFF";
    put32(file, static_cast<std::uint32_t>(body.size()));
    return file + body;
}
bool all_near(const std::vector<float>& s, float value) {
    for (const float v : s)
        if (std::abs(v - value) > 1e-4F) return false;
    return !s.empty();
}

// Every WAV sample format reads as the same level, mono, at 48 kHz.
void wav_formats() {
    std::string s16, s24, s32, f32;
    put16(s16, 16384);
    s24 = std::string("\0\0\x40", 3);
    put32(s32, 0x40000000);
    float quarter = 0.25F;
    std::uint32_t bits{};
    std::memcpy(&bits, &quarter, 4);
    put32(f32, bits);
    check(all_near(seed::decode_sound(wav(1, 8, 1, 48000, 100, std::string(1, '\xC0'))), 0.5F), "8-bit PCM");
    check(all_near(seed::decode_sound(wav(1, 16, 2, 48000, 100, s16)), 0.5F), "16-bit stereo PCM");
    check(all_near(seed::decode_sound(wav(1, 24, 1, 48000, 100, s24)), 0.5F), "24-bit PCM");
    check(all_near(seed::decode_sound(wav(1, 32, 1, 48000, 100, s32)), 0.5F), "32-bit PCM");
    check(all_near(seed::decode_sound(wav(3, 32, 1, 48000, 100, f32)), 0.25F), "32-bit float");
    check(all_near(seed::decode_sound(wav(1, 16, 1, 48000, 100, s16, {}, true)), 0.5F), "Extensible format");
    std::string odd = "LIST";
    put32(odd, 3);
    odd += std::string("abc\0", 4); // An odd-sized chunk, padded to even.
    check(all_near(seed::decode_sound(wav(1, 16, 1, 48000, 100, s16, odd)), 0.5F),
          "Padded chunks are skipped");
    // 44.1 kHz becomes 48 kHz: 4,410 frames span 0.1 s.
    const auto resampled = seed::decode_sound(wav(1, 16, 1, 44100, 4410, s16));
    check(resampled.size() == 4800 && all_near(resampled, 0.5F),
          "Resampled to 48 kHz: " + std::to_string(resampled.size()));
}

void wav_rejections() {
    std::string s16;
    put16(s16, 1000);
    const auto good = wav(1, 16, 1, 48000, 10, s16);
    rejects([&] { seed::decode_sound(good.substr(0, good.size() - 4)); }, "a cut-short file");
    rejects([&] { seed::decode_sound(wav(1, 16, 3, 48000, 10, s16)); }, "three channels");
    rejects([&] { seed::decode_sound(wav(1, 12, 1, 48000, 10, s16)); }, "12-bit samples");
    rejects([&] { seed::decode_sound(wav(2, 16, 1, 48000, 10, s16)); }, "ADPCM");
    rejects([&] { seed::decode_sound(wav(1, 16, 1, 1000, 10, s16)); }, "a 1 kHz rate");
    rejects([&] { seed::decode_sound(wav(1, 16, 1, 8000, 8000 * 11, s16)); }, "an 11 s sound effect");
    std::string nan;
    put32(nan, 0x7fc00000);
    rejects([&] { seed::decode_sound(wav(3, 32, 1, 48000, 4, nan)); }, "a NaN sample");
    rejects([&] { seed::decode_sound("ID3 not a sound file"); }, "an MP3");
    std::string no_data = "RIFF";
    put32(no_data, 4);
    no_data += "WAVE";
    rejects([&] { seed::decode_sound(no_data); }, "a WAV with no chunks");
}

// The Ogg fixture: one second of a 330 Hz tone at amplitude 0.37, stereo, 44.1 kHz.
void vorbis(const std::string& bytes) {
    const auto samples = seed::decode_sound(bytes);
    check(std::abs(static_cast<long>(samples.size()) - 48000) < 2000,
          "Ogg length: " + std::to_string(samples.size()));
    double energy = 0;
    int crossings = 0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        energy += samples[i] * samples[i];
        crossings += (samples[i - 1] < 0) != (samples[i] < 0);
    }
    const double rms = std::sqrt(energy / static_cast<double>(samples.size()));
    check(std::abs(rms - 0.37 / std::sqrt(2.0)) < 0.03, "Ogg level: " + std::to_string(rms));
    check(std::abs(crossings - 660) < 15, "Ogg pitch: " + std::to_string(crossings) + " crossings");
    rejects([&] { seed::decode_sound(bytes, 0.5); }, "an Ogg longer than the limit");
    rejects([&] { seed::decode_sound(bytes.substr(0, 60)); }, "a cut-short Ogg");

    std::vector<float> out(4096);
    seed::MusicStream once(bytes, false);
    std::size_t total = 0;
    while (!once.finished())
        total += once.read(out.data(), out.size());
    check(std::abs(static_cast<long>(total) - static_cast<long>(samples.size())) < 4,
          "Streamed music matches the whole decode: " + std::to_string(total));
    seed::MusicStream looping(bytes, true);
    for (int i = 0; i < 40; ++i)
        check(looping.read(out.data(), out.size()) == out.size(), "Looping music never ends");
    check(!looping.finished(), "Looping music is not finished");
    rejects([&] { seed::MusicStream bad(wav(1, 16, 1, 48000, 10, std::string(2, '\0')), true); },
            "WAV as music");
}
} // namespace

int main() {
    try {
        wav_formats();
        wav_rejections();
        vorbis(seed::read_text(SEED_AUDIO_FIXTURE));
        std::cout << "Audio file checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
