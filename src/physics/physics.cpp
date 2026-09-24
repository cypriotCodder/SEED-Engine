#include "physics/physics.hpp"
#include "io/binary.hpp"
#include "io/storage.hpp"
#include "world/generator.hpp"
#include <bit>
#include <span>

namespace seed {
namespace {
constexpr float dt = 1.0F / 60.0F;
constexpr std::size_t record_size = 81;
void floating(Bytes& out, float value) {
    out.u32(std::bit_cast<std::uint32_t>(value));
}
float floating(Reader& in) {
    const float value = std::bit_cast<float>(in.u32());
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite physics record");
    return value;
}
void position(Bytes& out, WorldPosition value) {
    out.u64(std::uint64_t(value.chunk.x));
    out.u64(std::uint64_t(value.chunk.y));
    floating(out, value.local.x);
    floating(out, value.local.y);
}
WorldPosition position(Reader& input) {
    WorldPosition result{{std::bit_cast<std::int64_t>(input.u64()), std::bit_cast<std::int64_t>(input.u64())},
                         {floating(input), floating(input)}};
    if (result.local.x < 0 || result.local.y < 0 || result.local.x >= chunk_side ||
        result.local.y >= chunk_side)
        throw std::runtime_error("Non-canonical physics position");
    return result;
}
} // namespace
std::size_t Physics::add(WorldPosition position, Vec2 half, bool fixed, float height) {
    if (count_ == capacity) throw std::runtime_error("Physics body budget exhausted");
    position.move({});
    auto& body = bodies_[count_];
    body.position = body.previous = position;
    body.half = half;
    body.inverse_mass = fixed ? 0.0F : 1.0F;
    body.height = body.previous_height = height;
    body.exists = true;
    body.entity =
        scene_.create({position, position, 0}, {fixed ? Material::stone : Material::wood, half * 2});
    return count_++;
}
void Physics::connect(std::size_t a, std::size_t b) {
    if (joint_count_ == joints_.size()) throw std::runtime_error("Building joint budget exhausted");
    joints_[joint_count_++] = {static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(b),
                               length(relative(bodies_[a].position, bodies_[b].position)), false};
}
Physics::Physics(Scene& scene, std::uint64_t seed, const std::filesystem::path& save)
    : scene_(scene), seed_(seed), file_(save / "0_0.bodies") {
    // Four anchored piers carry a small timber platform. The recipe is never saved.
    for (float y : {3.0F, 7.0F})
        for (float x : {-3.0F, 3.0F})
            add({{}, {x, y}}, {0.4F, 0.4F}, true, 0.9F);
    for (float y : {3.0F, 7.0F})
        for (float x : {-2.0F, 0.0F, 2.0F})
            add({{}, {x, y}}, {0.95F, 0.24F}, false, 0.9F);
    for (float x : {-3.0F, 3.0F})
        for (float y : {4.0F, 6.0F})
            add({{}, {x, y}}, {0.24F, 0.95F}, false, 0.9F);
    for (float x : {-2.0F, -1.0F, 0.0F, 1.0F, 2.0F})
        add({{}, {x, 5}}, {0.42F, 1.65F}, false, 0.9F);
    for (std::size_t i = 0; i < count_; ++i)
        for (std::size_t j = i + 1; j < count_; ++j)
            if (length(relative(bodies_[i].position, bodies_[j].position)) < 2.4F) connect(i, j);
    baseline_count_ = count_;
    for (std::size_t i = 0; i < count_; ++i) {
        const auto bytes = encode_body(bodies_[i]);
        baseline_.insert(baseline_.end(), bytes.begin(), bytes.end());
    }
    load();
    support();
    sync_scene();
}
std::vector<std::uint8_t> Physics::encode_body(const Body& b) const {
    Bytes out;
    out.u8(b.exists ? 1 : 0);
    position(out, b.position);
    position(out, b.previous);
    for (float f : {b.half.x, b.half.y, b.angle, b.previous_angle, b.height, b.previous_height,
                    b.inverse_mass, b.health})
        floating(out, f);
    return out.data;
}
void Physics::load() {
    if (!std::filesystem::exists(file_)) return;
    const auto bytes = read_blob(file_);
    Reader input(bytes);
    if (input.u32() != 0x59444f42) throw std::runtime_error("Invalid building save magic");
    const auto version = input.u32();
    if (version != 1 && version != 2) throw std::runtime_error("Unknown building save version");
    if (version == 2 && input.u32() != generator_version)
        throw std::runtime_error("Building generator version mismatch");
    if (input.u64() != seed_) throw std::runtime_error("Building seed mismatch");
    const auto total = input.u16(), changes = input.u16();
    if (total < baseline_count_ || total > capacity || changes > total)
        throw std::runtime_error("Invalid building counts");
    std::bitset<capacity> seen;
    for (unsigned n = 0; n < changes; ++n) {
        const auto i = input.u16();
        if (i >= total || seen[i]) throw std::runtime_error("Duplicate building record");
        seen[i] = true;
        auto& b = bodies_[i];
        const auto exists = input.u8();
        if (exists > 1) throw std::runtime_error("Invalid body flag");
        b.exists = exists != 0;
        b.position = position(input);
        b.previous = position(input);
        b.half = {floating(input), floating(input)};
        b.angle = floating(input);
        b.previous_angle = floating(input);
        b.height = floating(input);
        b.previous_height = floating(input);
        b.inverse_mass = floating(input);
        b.health = floating(input);
        if (b.half.x <= 0 || b.half.y <= 0 || b.half.x > 2 || b.half.y > 2 || b.height < 0 || b.height > 16 ||
            b.previous_height < 0 || b.previous_height > 16 || std::abs(b.angle) > 1000000 ||
            std::abs(b.previous_angle) > 1000000 || b.inverse_mass < 0 || b.inverse_mass > 1 ||
            b.health < 0 || b.health > 100 || (b.exists && b.health == 0) ||
            !nearby(b.position.chunk, b.previous.chunk, 1))
            throw std::runtime_error("Invalid physics state");
        if (i >= baseline_count_ && b.exists)
            b.entity = scene_.create({b.position, b.previous, b.angle}, {Material::wood, b.half * 2});
        else if (i < baseline_count_ && !b.exists)
            scene_.destroy(b.entity);
    }
    for (std::size_t i = baseline_count_; i < total; ++i)
        if (!seen[i]) throw std::runtime_error("Missing added body");
    count_ = total;
    const auto stored_joints = input.u16();
    if (version == 1) {
        if (stored_joints != joint_count_) throw std::runtime_error("Legacy building recipe mismatch");
        for (std::size_t i = 0; i < joint_count_; ++i) {
            auto& j = joints_[i];
            const auto a = input.u16(), b = input.u16();
            const auto rest = floating(input);
            const auto broken = input.u8();
            if (a != j.a || b != j.b || std::abs(rest - j.length) > 0.0001F || broken > 1)
                throw std::runtime_error("Invalid legacy joint");
            j.broken = broken != 0;
        }
    } else {
        if (stored_joints > joint_count_) throw std::runtime_error("Invalid broken joint count");
        std::bitset<1024> broken;
        for (unsigned i = 0; i < stored_joints; ++i) {
            const auto index = input.u16();
            if (index >= joint_count_ || broken[index])
                throw std::runtime_error("Invalid broken joint delta");
            broken[index] = true;
            joints_[index].broken = true;
        }
    }
    if (!input.done()) throw std::runtime_error("Trailing building data");
}
void Physics::save() {
    Bytes records;
    std::uint16_t changes = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        const auto bytes = encode_body(bodies_[i]);
        if (bytes.size() != record_size) throw std::logic_error("Physics record schema size mismatch");
        if (i >= baseline_count_ ||
            !std::equal(bytes.begin(), bytes.end(),
                        baseline_.begin() + static_cast<std::ptrdiff_t>(i * record_size))) {
            records.u16(static_cast<std::uint16_t>(i));
            records.data.insert(records.data.end(), bytes.begin(), bytes.end());
            ++changes;
        }
    }
    if (!changes && !std::filesystem::exists(file_)) return;
    Bytes out;
    out.u32(0x59444f42);
    out.u32(2);
    out.u32(generator_version);
    out.u64(seed_);
    out.u16(static_cast<std::uint16_t>(count_));
    out.u16(changes);
    out.data.insert(out.data.end(), records.data.begin(), records.data.end());
    std::uint16_t broken = 0;
    for (std::size_t i = 0; i < joint_count_; ++i)
        if (joints_[i].broken) ++broken;
    out.u16(broken);
    for (std::size_t i = 0; i < joint_count_; ++i)
        if (joints_[i].broken) out.u16(static_cast<std::uint16_t>(i));
    write_blob(file_, out.data);
}
void Physics::support() {
    for (std::size_t i = 0; i < count_; ++i)
        bodies_[i].supported = bodies_[i].exists && bodies_[i].inverse_mass == 0;
    for (std::size_t pass = 0; pass < count_; ++pass) {
        bool changed = false;
        for (std::size_t i = 0; i < joint_count_; ++i) {
            const auto& j = joints_[i];
            auto& a = bodies_[j.a];
            auto& b = bodies_[j.b];
            if (j.broken || !a.exists || !b.exists || a.supported == b.supported) continue;
            a.supported = b.supported = true;
            changed = true;
        }
        if (!changed) break;
    }
}
void Physics::contacts() {
    heads_.fill(-1);
    pairs_.reset();
    std::size_t used = 0;
    auto hash = [](int x, int y) {
        return (std::uint32_t(x) * 73856093U ^ std::uint32_t(y) * 19349663U) & 511U;
    };
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& b = bodies_[i];
        if (!active(b)) continue;
        const auto aabb = bounds({relative(b.position, anchor_), b.half, b.angle});
        const int x0 = static_cast<int>(std::floor(aabb.minimum.x / 4)),
                  x1 = static_cast<int>(std::floor(aabb.maximum.x / 4));
        const int y0 = static_cast<int>(std::floor(aabb.minimum.y / 4)),
                  y1 = static_cast<int>(std::floor(aabb.maximum.y / 4));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (used == entries_.size()) throw std::runtime_error("Spatial hash capacity exhausted");
                const auto h = hash(x, y);
                entries_[used] = {x, y, heads_[h], static_cast<std::uint16_t>(i)};
                heads_[h] = static_cast<int>(used++);
            }
    }
    for (std::size_t e = 0; e < used; ++e) {
        const auto& entry = entries_[e];
        for (int link = entry.next; link >= 0; link = entries_[static_cast<std::size_t>(link)].next) {
            const auto& other = entries_[static_cast<std::size_t>(link)];
            if (entry.x != other.x || entry.y != other.y || entry.body == other.body) continue;
            const auto low = std::min(entry.body, other.body), high = std::max(entry.body, other.body);
            const auto pair = low * capacity + high;
            if (pairs_[pair]) continue;
            pairs_[pair] = true;
            bool connected = false;
            for (std::size_t j = 0; j < joint_count_; ++j) {
                const auto& joint = joints_[j];
                if (!joint.broken &&
                    ((joint.a == low && joint.b == high) || (joint.a == high && joint.b == low))) {
                    connected = true;
                    break;
                }
            }
            if (connected) continue; // Joined timbers may intentionally intersect at a joint.
            auto& a = bodies_[low];
            auto& b = bodies_[high];
            const float mass = a.inverse_mass + b.inverse_mass;
            if (mass == 0 || std::abs(a.height - b.height) > 0.4F) continue;
            Contact c;
            if (!collide({relative(a.position, anchor_), a.half, a.angle},
                         {relative(b.position, anchor_), b.half, b.angle}, c))
                continue;
            const auto ra = c.point - relative(a.position, anchor_),
                       rb = c.point - relative(b.position, anchor_);
            const float ia = 3 * a.inverse_mass / dot(a.half, a.half),
                        ib = 3 * b.inverse_mass / dot(b.half, b.half);
            const float ca = cross(ra, c.normal), cb = cross(rb, c.normal);
            const float correction =
                std::max(0.0F, c.depth - 0.002F) * 0.7F / (mass + ca * ca * ia + cb * cb * ib);
            a.position.move(c.normal * (-correction * a.inverse_mass));
            b.position.move(c.normal * (correction * b.inverse_mass));
            a.angle -= ca * correction * ia;
            b.angle += cb * correction * ib;
        }
    }
}
void Physics::simulate() {
    support();
    for (std::size_t i = 0; i < count_; ++i) {
        auto& b = bodies_[i];
        if (!active(b) || b.inverse_mass == 0) continue;
        const auto velocity = relative(b.position, b.previous) * 0.985F;
        b.previous = b.position;
        b.position.move(velocity);
        const auto angular = (b.angle - b.previous_angle) * 0.97F;
        b.previous_angle = b.angle;
        b.angle += angular;
        if (!b.supported) {
            const float previous = b.height;
            b.height += (b.height - b.previous_height) * 0.98F - 9.81F * dt * dt;
            b.previous_height = previous;
            if (b.height < 0) {
                b.height = 0;
                b.previous_height = 0;
            }
        }
    }
    for (int iteration = 0; iteration < 6; ++iteration) {
        for (std::size_t i = 0; i < joint_count_; ++i) {
            auto& j = joints_[i];
            auto& a = bodies_[j.a];
            auto& b = bodies_[j.b];
            if (j.broken || !active(a) || !active(b)) continue;
            const auto delta = relative(b.position, a.position);
            const float distance = length(delta), mass = a.inverse_mass + b.inverse_mass;
            if (distance > j.length + 1.25F) {
                j.broken = true;
                continue;
            }
            if (distance < 1e-6F || mass == 0) continue;
            const auto correction = delta * ((distance - j.length) * 0.7F / (distance * mass));
            a.position.move(correction * a.inverse_mass);
            b.position.move(correction * (-b.inverse_mass));
        }
        contacts();
    }
}
void Physics::job(void* context) noexcept {
    auto& physics = *static_cast<Physics*>(context);
    try {
        physics.simulate();
    } catch (...) {
        physics.error_ = std::current_exception();
    }
}
void Physics::step(Jobs& jobs, WorldPosition anchor) {
    anchor_ = anchor;
    error_ = nullptr;
    jobs.submit({job, this});
    jobs.wait();
    if (error_) std::rethrow_exception(error_);
    sync_scene();
}
void Physics::sync_scene() {
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& b = bodies_[i];
        if (!b.exists) continue;
        auto& t = *scene_.transforms.find(b.entity);
        t.position = b.position;
        t.previous = b.previous;
        t.angle = b.angle;
        t.position.move({0, b.height * 0.35F});
        t.previous.move({0, b.previous_height * 0.35F});
    }
}
bool Physics::damage(WorldPosition target, float amount) {
    if (!std::isfinite(amount) || amount <= 0)
        throw std::invalid_argument("Damage must be positive and finite");
    for (std::size_t i = 0; i < count_; ++i) {
        auto& b = bodies_[i];
        if (!b.exists || !nearby(b.position.chunk, target.chunk, 1)) continue;
        const auto delta = rotate(relative(target, b.position), -b.angle);
        if (std::abs(delta.x) > b.half.x + 0.15F || std::abs(delta.y) > b.half.y + 0.15F) continue;
        b.health = std::max(0.0F, b.health - amount);
        if (b.health == 0) {
            b.exists = false;
            scene_.destroy(b.entity);
        } else if (b.inverse_mass > 0) {
            b.previous.move(normalized(delta) * (-0.06F));
            b.previous_angle -= 0.025F;
        }
        support();
        return true;
    }
    return false;
}
bool Physics::build(WorldPosition target) {
    if (count_ == capacity || blocks(target)) return false;
    add(target, {0.45F, 0.45F}, false, 0);
    return true;
}
bool Physics::blocks(WorldPosition position) const {
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& b = bodies_[i];
        if (!b.exists || !nearby(position.chunk, b.position.chunk, 1)) continue;
        Contact c;
        if (collide({relative(position, b.position), {0.25F, 0.25F}, 0}, {{}, b.half, b.angle}, c))
            return true;
    }
    return false;
}
void Physics::collapse_demo() {
    for (std::size_t i = 0; i < 4; ++i)
        if (bodies_[i].exists) damage(bodies_[i].position, 100);
}
unsigned Physics::unsupported() const {
    unsigned result = 0;
    for (std::size_t i = 0; i < count_; ++i)
        if (bodies_[i].exists && !bodies_[i].supported) ++result;
    return result;
}
unsigned Physics::grounded_unsupported() const {
    unsigned result = 0;
    for (std::size_t i = 0; i < count_; ++i)
        if (bodies_[i].exists && !bodies_[i].supported && bodies_[i].height <= 0.0001F) ++result;
    return result;
}
} // namespace seed
