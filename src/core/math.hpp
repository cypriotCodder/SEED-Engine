#pragma once
#include <algorithm>
#include <cmath>

namespace seed {
struct Vec2 {
    float x{}, y{};
    Vec2 operator+(Vec2 b) const { return {x+b.x,y+b.y}; }
    Vec2 operator-(Vec2 b) const { return {x-b.x,y-b.y}; }
    Vec2 operator*(float s) const { return {x*s,y*s}; }
    Vec2& operator+=(Vec2 b) { x+=b.x; y+=b.y; return *this; }
};
inline float dot(Vec2 a, Vec2 b) { return a.x*b.x+a.y*b.y; }
inline float length(Vec2 a) { return std::sqrt(dot(a,a)); }
inline Vec2 normalized(Vec2 a) { const auto l=length(a); return l>0 ? a*(1/l) : Vec2{}; }
inline Vec2 rotate(Vec2 a, float angle) {
    const float c=std::cos(angle), s=std::sin(angle); return {c*a.x-s*a.y,s*a.x+c*a.y};
}
} // namespace seed
