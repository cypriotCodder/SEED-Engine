#include "physics/physics.hpp"
#include "world/chunk_file.hpp"
#include <algorithm>
#include <chrono>
#include <limits>

namespace seed {
namespace {
constexpr float dt = 1.0F / 60.0F;
constexpr float cell_size = 4;
constexpr std::uint32_t hash_mask = 4095;

} // namespace

Physics::Physics(Scene& scene, Jobs& jobs, BodyVisuals visuals)
    : scene_(scene), visuals_(visuals), jobs_(jobs), storage_(std::make_unique<Storage>()) {
    // Free lists pop ascending slots first, keeping pool order deterministic.
    for (std::size_t i = 0; i < body_capacity; ++i)
        storage_->free_bodies[i] = static_cast<std::uint16_t>(body_capacity - 1 - i);
    for (std::size_t i = 0; i < joint_capacity; ++i)
        storage_->free_joints[i] = static_cast<std::uint16_t>(joint_capacity - 1 - i);
    free_body_count_ = body_capacity;
    free_joint_count_ = joint_capacity;
}

Physics::~Physics() {
    if (pending_) jobs_.wait(group_);
}

void Physics::require_idle() const {
    if (pending_) throw std::logic_error("Physics state accessed while a step is running");
}

ChunkHooks Physics::hooks() {
    ChunkHooks hooks;
    hooks.context = this;
    hooks.activate = [](void* context, ChunkCoord coord, Chunk& chunk) {
        static_cast<Physics*>(context)->attach(coord, chunk.bodies);
    };
    hooks.release = [](void* context, ChunkCoord coord, Chunk& chunk) {
        if (static_cast<Physics*>(context)->release(coord, chunk.bodies)) chunk.dirty = true;
    };
    hooks.store = [](void* context, ChunkCoord coord, Chunk& chunk) {
        if (static_cast<Physics*>(context)->store(coord, chunk.bodies)) chunk.dirty = true;
    };
    return hooks;
}

Physics::Resident* Physics::resident(ChunkCoord coord) {
    for (auto& entry : storage_->residents)
        if (entry.used && entry.coord == coord) return &entry;
    return nullptr;
}
const Physics::Resident* Physics::resident(ChunkCoord coord) const {
    for (const auto& entry : storage_->residents)
        if (entry.used && entry.coord == coord) return &entry;
    return nullptr;
}

std::uint16_t Physics::allocate_body(const BodyState& state, ChunkCoord owner, std::uint16_t id,
                                     std::uint8_t resident) {
    if (!free_body_count_) throw std::runtime_error("Physics body budget exhausted");
    // Create the entity first: if the scene is full, no pool slot has been taken.
    const auto entity =
        scene_.create({state.position, state.previous, state.angle},
                      visuals_.visual ? visuals_.visual(visuals_.context, state) : Visual{0, state.half * 2});
    const auto index = storage_->free_bodies[--free_body_count_];
    auto& body = storage_->bodies[index];
    body = Body{};
    body.state = state;
    body.owner = owner;
    body.id = id;
    body.resident = resident;
    body.live = true;
    body.entity = entity;
    body_end_ = std::max<std::size_t>(body_end_, index + 1U);
    support_dirty_ = true;
    return index;
}

void Physics::free_body(std::uint16_t index) {
    auto& body = storage_->bodies[index];
    if (body.state.exists) scene_.destroy(body.entity);
    body.live = false;
    body.link_count = 0;
    storage_->free_bodies[free_body_count_++] = index;
}

void Physics::link(std::uint16_t body, std::uint16_t joint) {
    auto& target = storage_->bodies[body];
    if (target.link_count == links_per_body) throw std::runtime_error("Too many joints on one building body");
    target.links[target.link_count++] = joint;
}

void Physics::attach(ChunkCoord owner, const ChunkBodies& bodies) {
    require_idle();
    if (resident(owner)) throw std::logic_error("Chunk bodies attached twice");
    auto slot = std::find_if(storage_->residents.begin(), storage_->residents.end(),
                             [](const Resident& entry) { return !entry.used; });
    if (slot == storage_->residents.end())
        throw std::runtime_error("Physics resident chunk budget exhausted");
    const auto slot_index = static_cast<std::uint8_t>(slot - storage_->residents.begin());

    // Check every budget before changing anything, so a rejected chunk leaves no trace and can be
    // retried once capacity frees up.
    if (bodies.count > chunk_body_capacity || bodies.recipe_count > bodies.count ||
        bodies.joint_count > chunk_joint_capacity)
        throw std::runtime_error("Invalid chunk body counts");
    std::size_t needed_bodies = 0, needed_joints = 0;
    std::array<std::uint8_t, chunk_body_capacity> degree{};
    for (std::uint16_t i = 0; i < bodies.count; ++i)
        if (bodies.bodies[i].exists) ++needed_bodies;
    for (std::uint16_t i = 0; i < bodies.joint_count; ++i) {
        const auto& recipe = bodies.joints[i];
        if (recipe.a >= bodies.recipe_count || recipe.b >= bodies.recipe_count || recipe.a == recipe.b)
            throw std::runtime_error("Invalid joint recipe");
        if (bodies.broken[i] || !bodies.bodies[recipe.a].exists || !bodies.bodies[recipe.b].exists) continue;
        ++needed_joints;
        if (++degree[recipe.a] > links_per_body || ++degree[recipe.b] > links_per_body)
            throw std::runtime_error("Too many joints on one building body");
    }
    if (needed_bodies > free_body_count_) throw std::runtime_error("Physics body budget exhausted");
    if (needed_joints > free_joint_count_) throw std::runtime_error("Physics joint budget exhausted");

    // Scene allocation can still fail; undo everything built so far if it does.
    std::array<std::uint16_t, chunk_body_capacity> pool_index;
    pool_index.fill(none);
    try {
        for (std::uint16_t i = 0; i < bodies.count; ++i)
            if (bodies.bodies[i].exists)
                pool_index[i] = allocate_body(bodies.bodies[i], owner, i, slot_index);
    } catch (...) {
        for (std::size_t i = bodies.count; i-- > 0;)
            if (pool_index[i] != none) free_body(pool_index[i]);
        throw;
    }
    for (std::uint16_t i = 0; i < bodies.joint_count; ++i) {
        const auto& recipe = bodies.joints[i];
        const auto a = pool_index[recipe.a], b = pool_index[recipe.b];
        if (bodies.broken[i] || a == none || b == none) continue;
        const auto index = storage_->free_joints[--free_joint_count_];
        storage_->joints[index] = {owner, a, b, i, recipe.length, true, false};
        joint_end_ = std::max<std::size_t>(joint_end_, index + 1U);
        link(a, index);
        link(b, index);
    }
    // Publish residency last.
    *slot = {owner, bodies.recipe_count, true, false, false};
    support_dirty_ = true;
}

bool Physics::store(ChunkCoord owner, ChunkBodies& bodies) const {
    require_idle();
    const auto* entry = resident(owner);
    if (!entry) throw std::logic_error("Storing a chunk that is not resident");
    bool changed = false;
    auto copy = [&changed](const BodyState& from, BodyState& to) {
        if (encode_body(from) == encode_body(to)) return;
        to = from;
        changed = true;
    };

    // Built bodies are written after the recipe in a stable order: loaded ones by their previous
    // index, then newly built ones by pool slot. Destroyed built bodies are dropped.
    std::array<std::uint16_t, chunk_body_capacity> built{};
    std::size_t built_count = 0;
    for (std::size_t i = 0; i < body_end_; ++i) {
        const auto& body = storage_->bodies[i];
        if (!body.live || !(body.owner == owner)) continue;
        if (body.id < entry->recipe_count) {
            copy(body.state, bodies.bodies[body.id]);
        } else if (body.state.exists) {
            if (entry->recipe_count + built_count == chunk_body_capacity)
                throw std::logic_error("Built bodies exceed the chunk budget");
            built[built_count++] = static_cast<std::uint16_t>(i);
        }
    }
    std::stable_sort(built.begin(), built.begin() + static_cast<std::ptrdiff_t>(built_count),
                     [this](auto a, auto b) { return storage_->bodies[a].id < storage_->bodies[b].id; });
    const auto count = static_cast<std::uint16_t>(entry->recipe_count + built_count);
    if (count != bodies.count) changed = true;
    for (std::size_t k = 0; k < built_count; ++k) {
        auto& target = bodies.bodies[entry->recipe_count + k];
        if (entry->recipe_count + k >= bodies.count) target = BodyState{};
        copy(storage_->bodies[built[k]].state, target);
    }
    bodies.count = count;

    for (std::size_t i = 0; i < joint_end_; ++i) {
        const auto& joint = storage_->joints[i];
        if (!joint.live || !joint.broken || !(joint.owner == owner) || bodies.broken[joint.recipe]) continue;
        bodies.broken.set(joint.recipe);
        changed = true;
    }
    return changed;
}

bool Physics::release(ChunkCoord owner, ChunkBodies& bodies) {
    const bool changed = store(owner, bodies);
    // Free in descending order so the free lists hand slots back lowest-first, keeping the pool
    // order of a reloaded chunk the same as when it was first attached.
    for (std::size_t i = body_end_; i-- > 0;) {
        const auto& body = storage_->bodies[i];
        if (body.live && body.owner == owner) free_body(static_cast<std::uint16_t>(i));
    }
    for (std::size_t i = joint_end_; i-- > 0;) {
        auto& joint = storage_->joints[i];
        if (!joint.live || !(joint.owner == owner)) continue;
        joint.live = false;
        storage_->free_joints[free_joint_count_++] = static_cast<std::uint16_t>(i);
    }
    resident(owner)->used = false;
    support_dirty_ = true;
    return changed;
}

// Decides which resident chunks simulate and which only collide during this step.
void Physics::plan_step() {
    auto& residents = storage_->residents;
    for (auto& entry : residents) {
        entry.collide = entry.used && nearby(entry.coord, anchor_.chunk, simulation_radius + 1);
        entry.simulate = entry.used && nearby(entry.coord, anchor_.chunk, simulation_radius);
        for (std::int64_t dy = -1; dy <= 1 && entry.simulate; ++dy)
            for (std::int64_t dx = -1; dx <= 1 && entry.simulate; ++dx)
                entry.simulate = resident({checked_add(entry.coord.x, dx), checked_add(entry.coord.y, dy)});
    }
}

bool Physics::active(const Body& body) const {
    return body.live && body.state.exists && storage_->residents[body.resident].simulate;
}

bool Physics::collider(const Body& body) const {
    return body.live && body.state.exists && storage_->residents[body.resident].collide;
}

bool Physics::joined(std::uint16_t a, std::uint16_t b) const {
    const auto& body = storage_->bodies[a];
    for (std::uint8_t i = 0; i < body.link_count; ++i) {
        const auto& joint = storage_->joints[body.links[i]];
        if (!joint.broken && (joint.a == b || joint.b == b)) return true;
    }
    return false;
}

// Breadth-first search from anchored bodies across intact joints. Reruns only after damage,
// joint breakage or residency changes.
void Physics::support() {
    auto& bodies = storage_->bodies;
    auto& queue = storage_->queue;
    std::size_t head = 0, tail = 0;
    for (std::size_t i = 0; i < body_end_; ++i) {
        auto& body = bodies[i];
        body.supported = body.live && body.state.exists && body.state.inverse_mass == 0;
        if (body.supported) queue[tail++] = static_cast<std::uint16_t>(i);
    }
    while (head < tail) {
        const auto& body = bodies[queue[head++]];
        for (std::uint8_t i = 0; i < body.link_count; ++i) {
            const auto& joint = storage_->joints[body.links[i]];
            if (joint.broken) continue;
            auto& other = bodies[joint.a == queue[head - 1] ? joint.b : joint.a];
            if (!other.state.exists || other.supported) continue;
            other.supported = true;
            queue[tail++] = joint.a == queue[head - 1] ? joint.b : joint.a;
        }
    }
    support_dirty_ = false;
}

void Physics::contacts() {
    auto& s = *storage_;
    s.heads.fill(-1);
    std::size_t used = 0;
    auto hash = [](int x, int y) {
        return (std::uint32_t(x) * 73856093U ^ std::uint32_t(y) * 19349663U) & hash_mask;
    };
    for (std::size_t i = 0; i < body_end_; ++i) {
        const auto& body = s.bodies[i];
        if (!collider(body)) continue;
        const auto aabb = bounds({relative(body.state.position, anchor_), body.state.half, body.state.angle});
        const int x0 = static_cast<int>(std::floor(aabb.minimum.x / cell_size));
        const int x1 = static_cast<int>(std::floor(aabb.maximum.x / cell_size));
        const int y0 = static_cast<int>(std::floor(aabb.minimum.y / cell_size));
        const int y1 = static_cast<int>(std::floor(aabb.maximum.y / cell_size));
        s.first_cell[i] = {x0, y0};
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (used == s.entries.size()) throw std::runtime_error("Spatial hash capacity exhausted");
                const auto h = hash(x, y);
                s.entries[used] = {x, y, s.heads[h], static_cast<std::uint16_t>(i)};
                s.heads[h] = static_cast<int>(used++);
            }
    }
    for (std::size_t e = 0; e < used; ++e) {
        const auto& entry = s.entries[e];
        for (int link = entry.next; link >= 0; link = s.entries[static_cast<std::size_t>(link)].next) {
            const auto& other = s.entries[static_cast<std::size_t>(link)];
            if (entry.x != other.x || entry.y != other.y || entry.body == other.body) continue;
            const auto low = std::min(entry.body, other.body), high = std::max(entry.body, other.body);
            // Two bodies can share several cells. Handle the pair only in the first cell of their
            // overlap, so it is resolved exactly once without a pair table.
            const int reference_x = std::max(s.first_cell[low][0], s.first_cell[high][0]);
            const int reference_y = std::max(s.first_cell[low][1], s.first_cell[high][1]);
            if (entry.x != reference_x || entry.y != reference_y) continue;
            if (joined(low, high)) continue; // Joined timbers may intentionally intersect at a joint.

            auto& a = s.bodies[low].state;
            auto& b = s.bodies[high].state;
            // Bodies that are not simulated this step act as immovable obstacles.
            const float mass_a = active(s.bodies[low]) ? a.inverse_mass : 0.0F;
            const float mass_b = active(s.bodies[high]) ? b.inverse_mass : 0.0F;
            const float mass = mass_a + mass_b;
            if (mass == 0 || std::abs(a.height - b.height) > 0.4F) continue;
            Contact c;
            const auto pa = relative(a.position, anchor_), pb = relative(b.position, anchor_);
            if (!collide({pa, a.half, a.angle}, {pb, b.half, b.angle}, c)) continue;
            const auto ra = c.point - pa, rb = c.point - pb;
            const float ia = 3 * mass_a / dot(a.half, a.half);
            const float ib = 3 * mass_b / dot(b.half, b.half);
            const float ca = cross(ra, c.normal), cb = cross(rb, c.normal);
            const float correction =
                std::max(0.0F, c.depth - 0.002F) * 0.7F / (mass + ca * ca * ia + cb * cb * ib);
            a.position.move(c.normal * (-correction * mass_a));
            b.position.move(c.normal * (correction * mass_b));
            a.angle -= ca * correction * ia;
            b.angle += cb * correction * ib;
        }
    }
}

void Physics::simulate() {
    auto& s = *storage_;
    plan_step();
    if (support_dirty_) support();
    for (std::size_t i = 0; i < body_end_; ++i) {
        auto& body = s.bodies[i];
        auto& b = body.state;
        if (!active(body) || b.inverse_mass == 0) continue;
        const auto velocity = relative(b.position, b.previous) * 0.985F;
        b.previous = b.position;
        b.position.move(velocity);
        const auto angular = (b.angle - b.previous_angle) * 0.97F;
        b.previous_angle = b.angle;
        b.angle += angular;
        if (!body.supported) {
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
        for (std::size_t i = 0; i < joint_end_; ++i) {
            auto& joint = s.joints[i];
            if (!joint.live || joint.broken) continue;
            auto& body_a = s.bodies[joint.a];
            auto& body_b = s.bodies[joint.b];
            if (!active(body_a) || !active(body_b)) continue;
            auto& a = body_a.state;
            auto& b = body_b.state;
            const auto delta = relative(b.position, a.position);
            const float distance = length(delta), mass = a.inverse_mass + b.inverse_mass;
            if (distance > joint.length + 1.25F) {
                joint.broken = true;
                support_dirty_ = true;
                continue;
            }
            if (distance < 1e-6F || mass == 0) continue;
            const auto correction = delta * ((distance - joint.length) * 0.7F / (distance * mass));
            a.position.move(correction * a.inverse_mass);
            b.position.move(correction * (-b.inverse_mass));
        }
        contacts();
    }
    // Ownership is fixed, so a body may not wander beyond its owner's neighbourhood. Holding it at
    // its last valid position keeps every saved state loadable.
    for (std::size_t i = 0; i < body_end_; ++i) {
        auto& body = s.bodies[i];
        if (!active(body) || within_owner_reach(body.owner, body.state)) continue;
        body.state.position = body.state.previous;
        body.state.angle = body.state.previous_angle;
    }
}

void Physics::job(void* context) noexcept {
    auto& physics = *static_cast<Physics*>(context);
    try {
        const auto begin = std::chrono::steady_clock::now();
        physics.simulate();
        physics.last_step_ns_ = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin)
                .count());
    } catch (...) {
        physics.error_ = std::current_exception();
    }
}

void Physics::begin_step(WorldPosition anchor) {
    require_idle();
    anchor_ = anchor;
    error_ = nullptr;
    pending_ = true;
    try {
        jobs_.submit({job, this, &group_});
    } catch (...) {
        pending_ = false;
        throw;
    }
}

void Physics::finish_step() {
    if (!pending_) return;
    jobs_.wait(group_);
    pending_ = false;
    if (error_) std::rethrow_exception(error_);
    sync_scene();
}

void Physics::sync_scene() {
    for (std::size_t i = 0; i < body_end_; ++i) {
        const auto& body = storage_->bodies[i];
        if (!body.live || !body.state.exists) continue;
        const auto& b = body.state;
        auto& t = *scene_.transforms.find(body.entity);
        t.position = b.position;
        t.previous = b.previous;
        t.angle = b.angle;
        t.position.move({0, b.height * visuals_.lift_per_height});
        t.previous.move({0, b.previous_height * visuals_.lift_per_height});
    }
}

bool Physics::damage(WorldPosition target, float amount) {
    require_idle();
    if (!std::isfinite(amount) || amount <= 0)
        throw std::invalid_argument("Damage must be positive and finite");
    // Overlapping pieces can all cover the target; hit the one whose centre is closest, so the
    // result does not depend on pool order.
    std::size_t hit = body_capacity;
    float best = std::numeric_limits<float>::max();
    Vec2 hit_delta{};
    for (std::size_t i = 0; i < body_end_; ++i) {
        const auto& body = storage_->bodies[i];
        const auto& b = body.state;
        if (!body.live || !b.exists || !nearby(b.position.chunk, target.chunk, 1)) continue;
        const auto delta = rotate(relative(target, b.position), -b.angle);
        if (std::abs(delta.x) > b.half.x + 0.15F || std::abs(delta.y) > b.half.y + 0.15F) continue;
        if (dot(delta, delta) >= best) continue;
        best = dot(delta, delta);
        hit = i;
        hit_delta = delta;
    }
    if (hit == body_capacity) return false;
    auto& body = storage_->bodies[hit];
    auto& b = body.state;
    b.health = std::max(0.0F, b.health - amount);
    if (b.health == 0) {
        b.exists = false;
        scene_.destroy(body.entity);
        // Built bodies have no joints and no save record once destroyed, so their slot returns
        // to the pool now. Destroyed recipe bodies stay until unload: their tombstone is saved.
        if (body.id >= storage_->residents[body.resident].recipe_count)
            free_body(static_cast<std::uint16_t>(hit));
    } else if (b.inverse_mass > 0) {
        b.previous.move(normalized(hit_delta) * (-0.06F));
        b.previous_angle -= 0.025F;
    }
    support_dirty_ = true;
    return true;
}

bool Physics::build(WorldPosition target) {
    require_idle();
    target.move({});
    const auto* entry = resident(target.chunk);
    if (!entry || blocks(target)) return false;
    std::size_t built = 0;
    for (std::size_t i = 0; i < body_end_; ++i) {
        const auto& body = storage_->bodies[i];
        if (body.live && body.owner == target.chunk && body.id >= entry->recipe_count && body.state.exists)
            ++built;
    }
    if (entry->recipe_count + built >= chunk_body_capacity || !free_body_count_) return false;
    BodyState state;
    state.position = state.previous = target;
    state.half = {0.45F, 0.45F};
    allocate_body(state, target.chunk, built_id,
                  static_cast<std::uint8_t>(entry - storage_->residents.data()));
    return true;
}

bool Physics::blocks(WorldPosition position, Vec2 half) const {
    require_idle();
    for (std::size_t i = 0; i < body_end_; ++i) {
        const auto& body = storage_->bodies[i];
        const auto& b = body.state;
        if (!body.live || !b.exists || !nearby(position.chunk, b.position.chunk, 1)) continue;
        Contact c;
        if (collide({relative(position, b.position), half, 0}, {{}, b.half, b.angle}, c)) return true;
    }
    return false;
}

bool Physics::destroy(ChunkCoord owner, std::uint16_t recipe_id) {
    require_idle();
    const auto* entry = resident(owner);
    if (!entry || recipe_id >= entry->recipe_count) return false;
    for (std::size_t i = 0; i < body_end_; ++i) {
        auto& body = storage_->bodies[i];
        if (!body.live || !body.state.exists || !(body.owner == owner) || body.id != recipe_id) continue;
        body.state.health = 0;
        body.state.exists = false;
        scene_.destroy(body.entity);
        support_dirty_ = true;
        return true;
    }
    return false;
}

std::optional<WorldPosition> Physics::find(ChunkCoord owner, std::uint16_t recipe_id) const {
    require_idle();
    const auto* entry = resident(owner);
    if (!entry || recipe_id >= entry->recipe_count) return std::nullopt;
    for (std::size_t i = 0; i < body_end_; ++i) {
        const auto& body = storage_->bodies[i];
        if (body.live && body.state.exists && body.owner == owner && body.id == recipe_id)
            return body.state.position;
    }
    return std::nullopt;
}

std::size_t Physics::count() const {
    require_idle();
    std::size_t result = 0;
    for (std::size_t i = 0; i < body_end_; ++i)
        if (storage_->bodies[i].live && storage_->bodies[i].state.exists) ++result;
    return result;
}

unsigned Physics::unsupported() const {
    require_idle();
    unsigned result = 0;
    for (std::size_t i = 0; i < body_end_; ++i) {
        const auto& body = storage_->bodies[i];
        if (body.live && body.state.exists && !body.supported) ++result;
    }
    return result;
}

unsigned Physics::grounded_unsupported() const {
    require_idle();
    unsigned result = 0;
    for (std::size_t i = 0; i < body_end_; ++i) {
        const auto& body = storage_->bodies[i];
        if (body.live && body.state.exists && !body.supported && body.state.height <= 0.0001F) ++result;
    }
    return result;
}
} // namespace seed
