#include "io/checkpoint.hpp"
#include "io/binary.hpp"
#include "io/storage.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>

#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace seed {
namespace {
constexpr std::uint32_t pointer_magic = 0x52545043, manifest_magic = 0x464e4d43;
constexpr std::uint32_t maximum_files = 100000;
struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};
bool delta_name(const std::string& name) {
    if (name == "world.seed" || name == "player.delta") return true;
    if (!name.ends_with(".chunk")) return false;
    const auto split = name.find('_');
    if (split == std::string::npos) return false;
    const auto number = [](std::string_view value) {
        std::int64_t n{};
        const auto r = std::from_chars(value.data(), value.data() + value.size(), n);
        return r.ec == std::errc{} && r.ptr == value.data() + value.size() && std::to_string(n) == value;
    };
    return number(std::string_view(name).substr(0, split)) &&
           number(std::string_view(name).substr(split + 1, name.size() - split - 7));
}
std::vector<std::string> files(const std::filesystem::path& directory) {
    std::vector<std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (!delta_name(name)) continue;
        if (!std::filesystem::is_regular_file(entry.symlink_status()))
            throw std::runtime_error("Save delta must be a regular file");
        result.push_back(name);
        if (result.size() > maximum_files) throw std::runtime_error("Too many save delta files");
    }
    std::sort(result.begin(), result.end());
    return result;
}
void header(Reader& r, std::uint32_t magic) {
    if (r.u32() != magic) throw std::runtime_error("Invalid checkpoint record");
    if (r.u32() != 1) throw Unsupported("Unsupported checkpoint version");
}
std::pair<std::uint64_t, std::uint64_t> pointer(const std::filesystem::path& path) {
    const auto bytes = read_blob(path);
    Reader r(bytes);
    header(r, pointer_magic);
    const auto current = r.u64(), previous = r.u64();
    if (!current || current == previous || !r.done()) throw std::runtime_error("Invalid checkpoint pointer");
    return {current, previous};
}
void write_pointer(const std::filesystem::path& path, std::uint64_t current, std::uint64_t previous) {
    Bytes b;
    b.u32(pointer_magic);
    b.u32(1);
    b.u64(current);
    b.u64(previous);
    write_blob(path, b.data);
}
std::filesystem::path directory(const std::filesystem::path& root, std::uint64_t id) {
    return root / "checkpoints" / std::to_string(id);
}
void validate(const std::filesystem::path& path, std::uint64_t id) {
    if (std::filesystem::exists(path / "0_0.bodies"))
        throw Unsupported("Legacy standalone building saves are unsupported; choose a new save directory");
    if (!std::filesystem::is_directory(std::filesystem::symlink_status(path)))
        throw std::runtime_error("Invalid checkpoint directory");
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path / "manifest")))
        throw std::runtime_error("Invalid checkpoint manifest");
    const auto bytes = read_blob(path / "manifest");
    Reader r(bytes);
    header(r, manifest_magic);
    if (r.u64() != id) throw std::runtime_error("Checkpoint identity mismatch");
    const auto count = r.u32();
    if (!count || count > maximum_files) throw std::runtime_error("Invalid checkpoint file count");
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto length = r.u16();
        if (!length || length > 80) throw std::runtime_error("Invalid checkpoint filename");
        const auto text = r.take(length);
        const std::string name(text.begin(), text.end());
        if (!delta_name(name) || (!names.empty() && name <= names.back()))
            throw std::runtime_error("Invalid checkpoint file list");
        names.push_back(name);
        const auto size = r.u32(), crc = r.u32();
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path / name)))
            throw std::runtime_error("Invalid checkpoint delta");
        const auto payload = read_blob(path / name);
        if (payload.size() != size || crc32(payload) != crc)
            throw std::runtime_error("Checkpoint delta mismatch");
    }
    if (!r.done() || !std::binary_search(names.begin(), names.end(), "world.seed") || names != files(path))
        throw std::runtime_error("Incomplete checkpoint");
}
} // namespace
struct Checkpoint::Lock {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit Lock(const std::filesystem::path& path) {
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Save is locked or inaccessible");
    }
    ~Lock() { CloseHandle(handle); }
#else
    int fd = -1;
    explicit Lock(const std::filesystem::path& path) {
        fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        if (fd < 0) throw std::runtime_error("Cannot open save lock");
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            fd = -1;
            throw std::runtime_error("Save is already open by another writer");
        }
    }
    ~Lock() {
        if (fd >= 0) ::close(fd);
    }
#endif
};
Checkpoint::Checkpoint(std::filesystem::path root) : root_(std::move(root)), working_(root_ / "working") {
    std::filesystem::create_directories(root_);
    lock_ = std::make_unique<Lock>(root_ / "writer.lock");
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(root_ / "checkpoints")))
        throw std::runtime_error("Checkpoint directory cannot be a symlink");
    std::filesystem::path source = root_;
    const bool has_pointer = std::filesystem::exists(root_ / "CURRENT");
    const bool has_recovery = std::filesystem::exists(root_ / "RECOVERY");
    if (!has_pointer && !has_recovery &&
        (!files(root_).empty() || std::filesystem::exists(root_ / "0_0.bodies")))
        throw Unsupported(
            "Legacy flat saves are unsupported; choose a new save directory. Existing files were preserved");
    if (has_pointer || has_recovery) {
        for (const auto* name : {"CURRENT", "RECOVERY"}) {
            if (current_) break;
            try {
                const auto [latest, previous] = pointer(root_ / name);
                for (const auto id : {latest, previous}) {
                    if (!id) continue;
                    try {
                        validate(directory(root_, id), id);
                        current_ = id;
                        recovered_ = std::string_view(name) != "CURRENT" || id != latest;
                        break;
                    } catch (const Unsupported&) {
                        throw;
                    } catch (const std::exception&) {}
                }
            } catch (const Unsupported&) {
                throw;
            } catch (const std::exception&) {}
        }
        if (!current_) throw std::runtime_error("No valid committed checkpoint; save left untouched");
        source = directory(root_, current_);
    }
    // Working state is never a committed save. An OS lock makes stale crash cleanup safe.
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(working_)))
        throw std::runtime_error("Working save directory cannot be a symlink");
    std::filesystem::remove_all(working_);
    std::filesystem::create_directories(working_);
    if (current_)
        for (const auto& name : files(source))
            snapshot_blob(source / name, working_ / name);
    sync_directory(working_);
    set_read_view(current_ ? source : std::filesystem::path{}, {});
}
struct Checkpoint::Snapshot {
    std::uint64_t id{};
    std::filesystem::path target;
    std::filesystem::path base;
};
Checkpoint::~Checkpoint() {
    if (pending_) jobs_->wait(group_);
}
std::filesystem::path Checkpoint::read_path(const std::string& filename) const {
    if (!delta_name(filename)) throw std::invalid_argument("Invalid save filename");
    const auto working = working_ / filename;
    if (std::filesystem::exists(working)) return working;
    std::lock_guard lock(read_mutex_);
    for (const auto& base : {read_view_.frozen, read_view_.previous}) {
        if (!base.empty() && std::filesystem::exists(base / filename)) return base / filename;
    }
    return working;
}
void Checkpoint::set_read_view(std::filesystem::path frozen, std::filesystem::path previous) {
    std::lock_guard lock(read_mutex_);
    read_view_ = {std::move(frozen), std::move(previous)};
}
Checkpoint::Metrics Checkpoint::metrics() const {
    if (pending_) throw std::logic_error("Checkpoint metrics requested before completion");
    return metrics_;
}
void Checkpoint::commit(Observer observer) {
    finish_commit();
    try {
        auto snapshot = prepare();
        publish(*snapshot, observer);
    } catch (...) {
        error_ = std::current_exception();
        throw;
    }
}
bool Checkpoint::begin_commit(Jobs& jobs, Observer observer) {
    if (pending_ && !finish_ready()) return false;
    snapshot_ = prepare();
    observer_ = observer;
    error_ = nullptr;
    jobs_ = &jobs;
    pending_ = true;
    try {
        jobs.submit({publish_job, this, &group_});
    } catch (...) {
        pending_ = false;
        snapshot_.reset();
        throw;
    }
    return true;
}
void Checkpoint::publish_job(void* context) noexcept {
    auto& checkpoint = *static_cast<Checkpoint*>(context);
    try {
        checkpoint.publish(*checkpoint.snapshot_, checkpoint.observer_);
    } catch (...) {
        checkpoint.error_ = std::current_exception();
    }
}
bool Checkpoint::finish_ready() {
    if (!pending_ || jobs_->busy(group_)) return false;
    finish_commit();
    return true;
}
void Checkpoint::finish_commit() {
    if (!pending_) return;
    jobs_->wait(group_);
    pending_ = false;
    snapshot_.reset();
    if (error_) std::rethrow_exception(error_);
}
std::unique_ptr<Checkpoint::Snapshot> Checkpoint::prepare() {
    if (error_) std::rethrow_exception(error_); // A failed session must reopen its last committed snapshot.
    const auto start = std::chrono::steady_clock::now();
    metrics_ = {};

    if (!std::filesystem::exists(read_path("world.seed")))
        throw std::runtime_error("Cannot commit a save without world metadata");
    const auto parent = root_ / "checkpoints";
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(parent)))
        throw std::runtime_error("Checkpoint directory cannot be a symlink");
    std::filesystem::create_directories(parent);
    // IDs are unique names only. CURRENT/RECOVERY define ordering, never the system clock.
    auto id = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    if (!id) id = 1;
    while (std::filesystem::exists(directory(root_, id)))
        ++id;
    const auto target = directory(root_, id);
    if (!std::filesystem::is_directory(std::filesystem::symlink_status(working_)))
        throw std::runtime_error("Working save path must be a directory, not a symlink");
    // World/player writers have joined. Rename freezes all dirty files in constant filesystem
    // operations; fresh writes atomically replace files in a new working directory.
    std::filesystem::rename(working_, target);
    std::filesystem::create_directory(working_);
    const auto base = current_ ? directory(root_, current_) : std::filesystem::path{};
    set_read_view(target, base);
    metrics_.snapshot_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return std::make_unique<Snapshot>(Snapshot{id, target, base});
}
void Checkpoint::publish(const Snapshot& snapshot, Observer observer) {
    const auto start = std::chrono::steady_clock::now();
    const auto& [id, target, base] = snapshot;
    metrics_.moved_files = files(target).size();
    if (!base.empty()) {
        for (const auto& name : files(base)) {
            if (std::filesystem::exists(target / name)) continue;
            if (snapshot_blob(base / name, target / name))
                ++metrics_.linked_files;
            else
                ++metrics_.copied_files;
        }
    }
    const auto names = files(target);
    const auto parent = root_ / "checkpoints";
    Bytes manifest;
    manifest.u32(manifest_magic);
    manifest.u32(1);
    manifest.u64(id);
    manifest.u32(static_cast<std::uint32_t>(names.size()));
    for (const auto& name : names) {
        // Validate the frozen snapshot on the worker. No recompression or data rewrite.
        const auto bytes = read_blob(target / name);
        manifest.u16(static_cast<std::uint16_t>(name.size()));
        for (const auto c : name)
            manifest.u8(static_cast<std::uint8_t>(c));
        manifest.u32(static_cast<std::uint32_t>(bytes.size()));
        manifest.u32(crc32(bytes));
    }
    if (observer) observer(Stage::files_written);
    write_blob(target / "manifest", manifest.data);
    sync_directory(parent);
    sync_directory(root_);
    if (observer) observer(Stage::manifest_written);
    // The fallback always points to a known-good snapshot, including after recovery.
    if (current_) write_pointer(root_ / "RECOVERY", current_, 0);
    if (observer) observer(Stage::recovery_written);
    write_pointer(root_ / "CURRENT", id, current_);
    const auto previous = current_;
    current_ = id;
    if (observer) observer(Stage::published);
    // Only numeric directories belonging to this checkpoint store are retired.
    for (const auto& entry : std::filesystem::directory_iterator(parent)) {
        const auto name = entry.path().filename().string();
        std::uint64_t old{};
        const auto r = std::from_chars(name.data(), name.data() + name.size(), old);
        if (r.ec == std::errc{} && r.ptr == name.data() + name.size() && std::to_string(old) == name &&
            old != current_ && old != previous && std::filesystem::is_directory(entry.symlink_status()))
            std::filesystem::remove_all(entry.path());
    }
    sync_directory(parent);
    metrics_.publication_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
} // namespace seed
