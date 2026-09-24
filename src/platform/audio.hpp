#pragma once
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace seed {
// A bounded SPSC event queue keeps locks and allocations out of the audio callback.
class Audio final {
public:
    explicit Audio(bool enabled=true) {
        if (!enabled) return;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)!=0) { SDL_Log("Audio disabled: %s",SDL_GetError()); return; }
        SDL_AudioSpec desired{}; desired.freq=48000; desired.format=AUDIO_F32SYS;
        desired.channels=1; desired.samples=512; desired.callback=callback; desired.userdata=this;
        device_=SDL_OpenAudioDevice(nullptr,0,&desired,nullptr,0);
        if (!device_) SDL_Log("Audio disabled: %s",SDL_GetError());
        else SDL_PauseAudioDevice(device_,0);
    }
    ~Audio() { if (device_) SDL_CloseAudioDevice(device_); }
    Audio(const Audio&)=delete;
    Audio& operator=(const Audio&)=delete;
    void impact() {
        if (!device_) return;
        const auto write=write_.load(std::memory_order_relaxed),next=(write+1)%queue_.size();
        if (next==read_.load(std::memory_order_acquire)) return;
        queue_[write]=140+static_cast<float>((sequence_++*37)%180); write_.store(next,std::memory_order_release);
    }
private:
    struct Voice { float phase{},gain{},frequency{}; };
    static void callback(void* context,Uint8* stream,int bytes) noexcept {
        auto& self=*static_cast<Audio*>(context); auto* output=reinterpret_cast<float*>(stream);
        auto read=self.read_.load(std::memory_order_relaxed);
        const auto write=self.write_.load(std::memory_order_acquire);
        while (read!=write) {
            auto& voice=self.voices_[self.next_voice_++%self.voices_.size()];
            voice={0,0.2F,self.queue_[read]}; read=(read+1)%self.queue_.size();
        }
        self.read_.store(read,std::memory_order_release);
        for (int sample=0;sample<bytes/static_cast<int>(sizeof(float));++sample) {
            float value=0;
            for (auto& voice:self.voices_) if (voice.gain>0.00001F) {
                self.noise_=self.noise_*1664525U+1013904223U;
                const float noise=static_cast<float>(self.noise_>>8)/8388608-1;
                value+=voice.gain*(0.75F*std::sin(voice.phase)+0.25F*noise);
                voice.phase+=6.2831853F*voice.frequency/48000;
                if (voice.phase>6.2831853F) voice.phase-=6.2831853F;
                voice.gain*=0.9991F;
            }
            output[sample]=std::clamp(value,-0.8F,0.8F);
        }
    }
    SDL_AudioDeviceID device_{};
    std::array<float,64> queue_{};
    std::array<Voice,16> voices_{};
    std::atomic<std::size_t> read_{0},write_{0};
    std::size_t next_voice_{};
    std::uint32_t sequence_{},noise_=1;
};
} // namespace seed
