#include "assets/sound_file.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

namespace seed {
namespace {
std::uint32_t u32(std::string_view b, std::size_t at) {
    return std::uint32_t(std::uint8_t(b[at])) | std::uint32_t(std::uint8_t(b[at + 1])) << 8 |
           std::uint32_t(std::uint8_t(b[at + 2])) << 16 | std::uint32_t(std::uint8_t(b[at + 3])) << 24;
}
std::uint16_t u16(std::string_view b, std::size_t at) {
    return static_cast<std::uint16_t>(std::uint8_t(b[at]) | std::uint8_t(b[at + 1]) << 8);
}

// Linear resampling of mono samples at `rate` to 48 kHz.
std::vector<float> resample(const std::vector<float>& in, unsigned rate) {
    if (rate == audio_rate || in.size() < 2) return in;
    const double step = static_cast<double>(rate) / audio_rate;
    // As many samples as the same duration takes at 48 kHz; the last ones hold the final sample.
    const auto count = static_cast<std::size_t>(std::llround(static_cast<double>(in.size()) / step));
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double p = static_cast<double>(i) * step;
        const auto k = std::min(static_cast<std::size_t>(p), in.size() - 1);
        const float t = static_cast<float>(p - static_cast<double>(k));
        out[i] = k + 1 < in.size() ? in[k] + (in[k + 1] - in[k]) * t : in[k];
    }
    return out;
}

std::vector<float> decode_wav(std::string_view b, double max_seconds) {
    if (b.size() < 12 || b.substr(8, 4) != "WAVE") throw std::runtime_error("Not a WAV file");
    if (u32(b, 4) + 8ULL > b.size()) throw std::runtime_error("WAV file is cut short");
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    std::string_view data;
    bool has_format = false, has_data = false;
    for (std::size_t at = 12; at + 8 <= b.size();) {
        const auto id = b.substr(at, 4);
        const std::size_t size = u32(b, at + 4);
        if (size > b.size() - at - 8) throw std::runtime_error("WAV chunk runs past the end of the file");
        if (id == "fmt ") {
            if (size < 16) throw std::runtime_error("WAV format chunk is too short");
            format = u16(b, at + 8);
            channels = u16(b, at + 10);
            rate = u32(b, at + 12);
            bits = u16(b, at + 22);
            if (format == 0xFFFE) { // Extensible: the real format is the sub-format's first two bytes.
                if (size < 26) throw std::runtime_error("WAV extensible format chunk is too short");
                format = u16(b, at + 32);
            }
            has_format = true;
        } else if (id == "data") {
            data = b.substr(at + 8, size);
            has_data = true;
        }
        at += 8 + size + (size & 1); // Chunks are padded to even sizes.
    }
    if (!has_format || !has_data) throw std::runtime_error("WAV file has no format or no data");
    if (channels < 1 || channels > 2) throw std::runtime_error("WAV sounds must be mono or stereo");
    if (rate < 8000 || rate > 192000) throw std::runtime_error("WAV sample rate must be 8 to 192 kHz");
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool floats = format == 3 && bits == 32;
    if (!pcm && !floats)
        throw std::runtime_error("WAV sounds must be 8, 16, 24 or 32-bit PCM, or 32-bit float");
    const std::size_t width = bits / 8u, frame = width * channels, frames = data.size() / frame;
    if (static_cast<double>(frames) / rate > max_seconds)
        throw std::runtime_error("Sound is longer than " + std::to_string(static_cast<int>(max_seconds)) +
                                 " seconds; use it as music");
    std::vector<float> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        float sum = 0;
        for (std::size_t c = 0; c < channels; ++c) {
            const std::size_t at = i * frame + c * width;
            float v = 0;
            if (floats) {
                const auto bitsof = u32(data, at);
                std::memcpy(&v, &bitsof, 4);
                if (!std::isfinite(v)) throw std::runtime_error("WAV file holds an invalid sample");
                v = std::clamp(v, -1.0F, 1.0F);
            } else if (bits == 8)
                v = (static_cast<float>(std::uint8_t(data[at])) - 128) / 128;
            else if (bits == 16)
                v = static_cast<float>(static_cast<std::int16_t>(u16(data, at))) / 32768;
            else if (bits == 24) {
                const auto raw = std::int32_t(std::uint32_t(std::uint8_t(data[at])) << 8 |
                                              std::uint32_t(std::uint8_t(data[at + 1])) << 16 |
                                              std::uint32_t(std::uint8_t(data[at + 2])) << 24);
                v = static_cast<float>(raw >> 8) / 8388608;
            } else
                v = static_cast<float>(static_cast<double>(static_cast<std::int32_t>(u32(data, at))) /
                                       2147483648.0);
            sum += v;
        }
        mono[i] = sum / channels;
    }
    return resample(mono, rate);
}

stb_vorbis* open_vorbis(std::string_view bytes) {
    if (bytes.size() > 0x7fffffff) throw std::runtime_error("Ogg file is too large");
    int error = 0;
    auto* v = stb_vorbis_open_memory(reinterpret_cast<const unsigned char*>(bytes.data()),
                                     static_cast<int>(bytes.size()), &error, nullptr);
    if (!v)
        throw std::runtime_error("Not an Ogg Vorbis file (stb_vorbis error " + std::to_string(error) + ")");
    const auto info = stb_vorbis_get_info(v);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate < 8000 || info.sample_rate > 192000) {
        stb_vorbis_close(v);
        throw std::runtime_error("Ogg sounds must be mono or stereo, at 8 to 192 kHz");
    }
    return v;
}

std::vector<float> decode_vorbis(std::string_view bytes, double max_seconds) {
    auto* v = open_vorbis(bytes);
    const auto info = stb_vorbis_get_info(v);
    const auto limit = static_cast<std::size_t>(max_seconds * info.sample_rate);
    std::vector<float> mono, buffer(4096 * static_cast<std::size_t>(info.channels));
    for (;;) {
        const int frames = stb_vorbis_get_samples_float_interleaved(v, info.channels, buffer.data(),
                                                                    static_cast<int>(buffer.size()));
        if (frames <= 0) break;
        for (int i = 0; i < frames; ++i) {
            float sum = 0;
            for (int c = 0; c < info.channels; ++c)
                sum += buffer[static_cast<std::size_t>(i * info.channels + c)];
            mono.push_back(std::clamp(sum / static_cast<float>(info.channels), -1.0F, 1.0F));
        }
        if (mono.size() > limit) {
            stb_vorbis_close(v);
            throw std::runtime_error("Sound is longer than " + std::to_string(static_cast<int>(max_seconds)) +
                                     " seconds; use it as music");
        }
    }
    stb_vorbis_close(v);
    return resample(mono, info.sample_rate);
}
} // namespace

std::vector<float> decode_sound(std::string_view bytes, double max_seconds) {
    if (bytes.size() >= 4 && bytes.substr(0, 4) == "RIFF") return decode_wav(bytes, max_seconds);
    if (bytes.size() >= 4 && bytes.substr(0, 4) == "OggS") return decode_vorbis(bytes, max_seconds);
    throw std::runtime_error("Sounds are WAV or Ogg Vorbis files");
}

MusicStream::MusicStream(std::string bytes, bool loop) : bytes_(std::move(bytes)), loop_(loop) {
    if (bytes_.size() < 4 || bytes_.substr(0, 4) != "OggS")
        throw std::runtime_error("Music must be Ogg Vorbis");
    vorbis_ = open_vorbis(bytes_);
    const auto info = stb_vorbis_get_info(vorbis_);
    channels_ = info.channels;
    step_ = static_cast<double>(info.sample_rate) / audio_rate;
    decoded_.resize(2048 * static_cast<std::size_t>(channels_));
    if (!next(a_) || !next(b_)) b_ = a_; // A one-sample file holds that sample.
}

MusicStream::~MusicStream() {
    if (vorbis_) stb_vorbis_close(vorbis_);
}

bool MusicStream::next(float& sample) {
    if (used_ == frames_) {
        frames_ = static_cast<std::size_t>(
            std::max(0, stb_vorbis_get_samples_float_interleaved(vorbis_, channels_, decoded_.data(),
                                                                 static_cast<int>(decoded_.size()))));
        used_ = 0;
        if (frames_ == 0 && loop_) {
            stb_vorbis_seek_start(vorbis_);
            frames_ = static_cast<std::size_t>(
                std::max(0, stb_vorbis_get_samples_float_interleaved(vorbis_, channels_, decoded_.data(),
                                                                     static_cast<int>(decoded_.size()))));
        }
        if (frames_ == 0) return false;
    }
    float sum = 0;
    for (int c = 0; c < channels_; ++c)
        sum += decoded_[used_ * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(c)];
    ++used_;
    sample = std::clamp(sum / static_cast<float>(channels_), -1.0F, 1.0F);
    return true;
}

std::size_t MusicStream::read(float* out, std::size_t count) {
    std::size_t written = 0;
    while (written < count && !finished_) {
        out[written++] = a_ + (b_ - a_) * static_cast<float>(fraction_);
        fraction_ += step_;
        while (fraction_ >= 1 && !finished_) {
            fraction_ -= 1;
            a_ = b_;
            if (!next(b_)) finished_ = true;
        }
    }
    return written;
}
} // namespace seed
