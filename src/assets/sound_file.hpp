#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct stb_vorbis;

namespace seed {
// Sound files a project imports: WAV (8, 16, 24 or 32-bit integer or 32-bit float PCM) and Ogg
// Vorbis, mono or stereo. Games play them as mono at the mixer's rate.
constexpr unsigned audio_rate = 48000;

// Decodes a whole sound effect to mono samples at 48 kHz. Throws for an unsupported or malformed
// file, or one longer than `max_seconds`.
std::vector<float> decode_sound(std::string_view bytes, double max_seconds = 10);

// Plays a long Ogg Vorbis file (music) as mono 48 kHz samples, decoding as it goes so only a
// little of it is ever decoded at once. Throws on construction if the file is not Ogg Vorbis.
class MusicStream final {
public:
    MusicStream(std::string bytes, bool loop);
    ~MusicStream();
    MusicStream(const MusicStream&) = delete;
    MusicStream& operator=(const MusicStream&) = delete;
    // Writes up to `count` samples; fewer only once a non-looping stream has ended.
    std::size_t read(float* out, std::size_t count);
    bool finished() const { return finished_; }

private:
    bool next(float& sample); // The next source sample, mixed to mono; false at the end.
    std::string bytes_;       // stb_vorbis reads from here.
    stb_vorbis* vorbis_{};
    int channels_{};
    double step_{}, fraction_{};
    float a_{}, b_{};
    std::vector<float> decoded_; // Interleaved source frames waiting to be used.
    std::size_t used_{}, frames_{};
    bool loop_{}, finished_{};
};
} // namespace seed
