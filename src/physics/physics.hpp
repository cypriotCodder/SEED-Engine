#pragma once
#include "physics/collision.hpp"
#include "core/scene.hpp"
#include "core/jobs.hpp"
#include <array>
#include <bitset>
#include <filesystem>

namespace seed {
class Physics final {
public:
    static constexpr std::size_t capacity=256;
    struct Body {
        WorldPosition position{},previous{};
        Vec2 half{0.5F,0.5F};
        float angle{},previous_angle{},height{},previous_height{},inverse_mass{1},health{100};
        bool exists{},supported{};
        Entity entity{};
    };
    Physics(Scene& scene,std::uint64_t seed,const std::filesystem::path& save);
    void step(Jobs& jobs,WorldPosition anchor);
    bool damage(WorldPosition target,float amount);
    bool build(WorldPosition target);
    void save();
    bool blocks(WorldPosition position) const;
    void collapse_demo();
    std::size_t count() const { return count_; }
    unsigned unsupported() const;
    unsigned grounded_unsupported() const;
private:
    struct Joint { std::uint16_t a{},b{}; float length{}; bool broken{}; };
    struct CellEntry { int x{},y{},next{-1}; std::uint16_t body{}; };
    static void job(void* context) noexcept;
    void simulate();
    std::size_t add(WorldPosition position,Vec2 half,bool fixed,float height);
    void connect(std::size_t a,std::size_t b);
    void sync_scene();
    void support();
    void contacts();
    void load();
    bool active(const Body& body) const { return body.exists && nearby(body.position.chunk,anchor_.chunk,3); }
    Scene& scene_;
    std::uint64_t seed_;
    std::filesystem::path file_;
    std::array<Body,capacity> bodies_{};
    std::array<Joint,1024> joints_{};
    std::array<int,512> heads_{};
    std::array<CellEntry,capacity*16> entries_{};
    std::bitset<capacity*capacity> pairs_;
    std::size_t count_{},joint_count_{},baseline_count_{};
    WorldPosition anchor_{};
    std::exception_ptr error_;
    std::vector<std::uint8_t> baseline_;
    std::vector<std::uint8_t> encode_body(const Body& body) const;
};
} // namespace seed
