#pragma once
#include "world/coordinates.hpp"
#include <array>
#include <utility>

namespace seed {
inline std::uint64_t mix64(std::uint64_t x) {
    x^=x>>30; x*=0xbf58476d1ce4e5b9ULL; x^=x>>27; x*=0x94d049bb133111ebULL; return x^(x>>31);
}
inline std::uint64_t world_hash(std::uint64_t seed,std::uint64_t x,std::uint64_t y) {
    return mix64(seed^mix64(x+0x9e3779b97f4a7c15ULL)^mix64(y+0xd1b54a32d192ed03ULL));
}
// Gradient Perlin noise. Integer lattice hashing retains precision at distant chunks.
inline float perlin(std::uint64_t seed,ChunkCoord chunk,Vec2 local,unsigned wavelength) {
    if (!std::isfinite(local.x) || !std::isfinite(local.y) || local.x<0 || local.y<0 || local.x>chunk_side || local.y>chunk_side)
        throw std::invalid_argument("Noise requires chunk-local coordinates in [0,32]");
    if (wavelength<1 || wavelength>1024 || (wavelength&(wavelength-1)))
        throw std::invalid_argument("Noise wavelength must be a power of two in [1,1024]");
    auto axis=[wavelength](std::int64_t c,float p) -> std::pair<std::uint64_t,float> {
        std::uint64_t lattice{};
        float fractional{};
        if (wavelength>=chunk_side) {
            const auto divisor=static_cast<std::int64_t>(wavelength/chunk_side);
            auto quotient=c/divisor, remainder=c%divisor;
            if (remainder<0) { --quotient; remainder+=divisor; }
            lattice=static_cast<std::uint64_t>(quotient);
            fractional=(static_cast<float>(remainder)*chunk_side+p)/static_cast<float>(wavelength);
        } else {
            lattice=static_cast<std::uint64_t>(c)*(chunk_side/wavelength);
            fractional=p/static_cast<float>(wavelength);
        }
        const auto integral=static_cast<std::int64_t>(std::floor(fractional));
        return {lattice+static_cast<std::uint64_t>(integral),fractional-static_cast<float>(integral)};
    };
    const auto [ix,x]=axis(chunk.x,local.x);
    const auto [iy,y]=axis(chunk.y,local.y);
    constexpr std::array<Vec2,8> gradients{{{1,0},{-1,0},{0,1},{0,-1},
        {0.70710678F,0.70710678F},{-0.70710678F,0.70710678F},{0.70710678F,-0.70710678F},{-0.70710678F,-0.70710678F}}};
    auto gradient=[&](unsigned dx,unsigned dy) { return dot(gradients[world_hash(seed,ix+dx,iy+dy)&7],{x-dx,y-dy}); };
    auto fade=[](float t) { return t*t*t*(t*(t*6-15)+10); };
    const float a=std::lerp(gradient(0,0),gradient(1,0),fade(x));
    const float b=std::lerp(gradient(0,1),gradient(1,1),fade(x));
    return std::lerp(a,b,fade(y));
}
inline float fractal(std::uint64_t seed,ChunkCoord chunk,Vec2 local) {
    float value=0, amplitude=0.55F;
    for (unsigned wavelength=128;wavelength>=8;wavelength/=2) {
        value+=perlin(seed,chunk,local,wavelength)*amplitude;
        amplitude*=0.5F; seed=mix64(seed);
    }
    return value;
}
} // namespace seed
