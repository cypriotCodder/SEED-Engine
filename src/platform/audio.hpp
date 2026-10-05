#pragma once
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace seed {
using SoundId = std::uint8_t;

// A sound: either synthesized (a decaying tone mixed with noise) or, when `samples` is set,
// recorded mono samples at 48 kHz, played at `gain`. Samples belong to the game and must stay
// unchanged while audio runs.
struct SoundDesc {
    const char* name{};
    float frequency{140}; // Base pitch in Hz.
    float variation{};    // Successive plays step through [frequency, frequency + variation) Hz.
    float gain{0.2F};     // Starting volume, 0-1.
    float decay{0.9991F}; // Volume multiplier per sample at 48 kHz (0.9991 fades in about 0.1 s).
    float tone{0.75F};    // Share of the sine tone; the rest is noise.
    const float* samples{};
    std::uint32_t sample_count{};
};

// The game's sounds, registered before audio starts. IDs follow registration order.
class Sounds final {
public:
    static constexpr std::size_t capacity = 32;
    SoundId add(const SoundDesc& desc) {
        if (!desc.name || !*desc.name) throw std::invalid_argument("A sound needs a name");
        if (!(desc.frequency > 0 && desc.frequency < 20000) || desc.variation < 0 || desc.gain < 0 ||
            desc.gain > 1 || !(desc.decay > 0 && desc.decay < 1) || desc.tone < 0 || desc.tone > 1)
            throw std::invalid_argument(std::string("Sound out of range: ") + desc.name);
        for (std::size_t i = 0; i < size_; ++i)
            if (std::string_view(entries_[i].name) == desc.name)
                throw std::invalid_argument(std::string("Duplicate sound: ") + desc.name);
        if (size_ == capacity) throw std::length_error("Too many sounds");
        entries_[size_] = desc;
        return static_cast<SoundId>(size_++);
    }
    SoundId find(std::string_view name) const {
        for (std::size_t i = 0; i < size_; ++i)
            if (entries_[i].name == name) return static_cast<SoundId>(i);
        throw std::out_of_range("Unknown sound: " + std::string(name));
    }
    const SoundDesc& operator[](SoundId id) const {
        if (id >= size_) throw std::out_of_range("Unregistered sound");
        return entries_[id];
    }
    std::size_t size() const { return size_; }

private:
    std::array<SoundDesc, capacity> entries_{};
    std::size_t size_{};
};

// Plays registered sounds. A bounded SPSC event queue keeps locks and allocations out of the audio
// callback; when the queue is full, new sounds are dropped.
class Audio final {
public:
    explicit Audio(bool enabled, const Sounds& sounds) : sounds_(sounds) {
        if (!enabled) return;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            SDL_Log("Audio disabled: %s", SDL_GetError());
            return;
        }
        SDL_AudioSpec desired{};
        desired.freq = 48000;
        desired.format = AUDIO_F32SYS;
        desired.channels = 1;
        desired.samples = 512;
        desired.callback = callback;
        desired.userdata = this;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
        if (!device_)
            SDL_Log("Audio disabled: %s", SDL_GetError());
        else
            SDL_PauseAudioDevice(device_, 0);
    }
    ~Audio() {
        if (device_) SDL_CloseAudioDevice(device_);
    }
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;
    void play(SoundId id) {
        play(sounds_[id]); // Validates the ID even when audio is off.
    }
    // Plays a sound that need not be registered, e.g. one being edited.
    void play(const SoundDesc& sound) {
        if (!device_) return;
        const auto write = write_.load(std::memory_order_relaxed), next = (write + 1) % queue_.size();
        if (next == read_.load(std::memory_order_acquire)) return;
        const auto spread = static_cast<std::uint32_t>(sound.variation);
        const float pitch =
            sound.frequency + (spread ? static_cast<float>((sequence_++ * 37) % spread) : 0.0F);
        queue_[write] = {pitch, sound.gain, sound.decay, sound.tone, sound.samples, sound.sample_count};
        write_.store(next, std::memory_order_release);
    }

    // Music: the main thread keeps a ring of mono samples filled (see MusicStream) that the
    // callback plays, so music never decodes on the audio thread. With audio off, music is
    // accepted and dropped.
    static constexpr std::size_t music_capacity = 16384; // About a third of a second.
    bool enabled() const { return device_ != 0; }
    // Room for samples in the ring now.
    std::size_t music_space() const {
        const auto read = music_read_.load(std::memory_order_acquire),
                   write = music_write_.load(std::memory_order_relaxed);
        return music_capacity - 1 - (write + music_capacity - read) % music_capacity;
    }
    void push_music(const float* samples, std::size_t count) {
        auto write = music_write_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < count; ++i) {
            music_[write] = samples[i];
            write = (write + 1) % music_capacity;
        }
        music_write_.store(write, std::memory_order_release);
    }
    // Drops whatever music is waiting, so a stopped song falls silent at once.
    void clear_music() { music_flush_.store(true, std::memory_order_release); }
    void set_music_volume(float volume) {
        music_volume_.store(std::clamp(volume, 0.0F, 1.0F), std::memory_order_relaxed);
    }

private:
    struct Event {
        float frequency{}, gain{}, decay{}, tone{};
        const float* samples{};
        std::uint32_t count{};
    };
    struct Voice {
        float phase{}, gain{}, frequency{}, decay{}, tone{};
        const float* samples{}; // A recorded sound, played from `position` to `count`.
        std::uint32_t count{}, position{};
    };
    static void callback(void* context, Uint8* stream, int bytes) noexcept {
        auto& self = *static_cast<Audio*>(context);
        auto* output = reinterpret_cast<float*>(stream);
        auto read = self.read_.load(std::memory_order_relaxed);
        const auto write = self.write_.load(std::memory_order_acquire);
        while (read != write) {
            auto& voice = self.voices_[self.next_voice_++ % self.voices_.size()];
            const auto& event = self.queue_[read];
            voice = {0, event.gain, event.frequency, event.decay, event.tone, event.samples, event.count, 0};
            read = (read + 1) % self.queue_.size();
        }
        self.read_.store(read, std::memory_order_release);
        auto music_read = self.music_read_.load(std::memory_order_relaxed);
        const auto music_write = self.music_write_.load(std::memory_order_acquire);
        if (self.music_flush_.exchange(false, std::memory_order_acq_rel)) music_read = music_write;
        const float music_volume = self.music_volume_.load(std::memory_order_relaxed);
        for (int sample = 0; sample < bytes / static_cast<int>(sizeof(float)); ++sample) {
            float value = 0;
            if (music_read != music_write) {
                value += self.music_[music_read] * music_volume;
                music_read = (music_read + 1) % music_capacity;
            }
            for (auto& voice : self.voices_)
                if (voice.samples) {
                    if (voice.position < voice.count)
                        value += voice.gain * voice.samples[voice.position++];
                    else
                        voice.samples = nullptr, voice.gain = 0;
                } else if (voice.gain > 0.00001F) {
                    self.noise_ = self.noise_ * 1664525U + 1013904223U;
                    const float noise = static_cast<float>(self.noise_ >> 8) / 8388608 - 1;
                    value += voice.gain * (voice.tone * std::sin(voice.phase) + (1 - voice.tone) * noise);
                    voice.phase += 6.2831853F * voice.frequency / 48000;
                    if (voice.phase > 6.2831853F) voice.phase -= 6.2831853F;
                    voice.gain *= voice.decay;
                }
            output[sample] = std::clamp(value, -0.8F, 0.8F);
        }
        self.music_read_.store(music_read, std::memory_order_release);
    }
    Sounds sounds_;
    SDL_AudioDeviceID device_{};
    std::array<Event, 64> queue_{};
    std::array<Voice, 16> voices_{};
    std::atomic<std::size_t> read_{0}, write_{0};
    std::size_t next_voice_{};
    std::uint32_t sequence_{}, noise_ = 1;
    std::array<float, music_capacity> music_{};
    std::atomic<std::size_t> music_read_{0}, music_write_{0};
    std::atomic<bool> music_flush_{false};
    std::atomic<float> music_volume_{0.6F};
};
} // namespace seed
