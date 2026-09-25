#pragma once
#include "core/jobs.hpp"
#include "io/binary.hpp"
#include "io/storage.hpp"
#include <filesystem>
#include <memory>
#include <string>

namespace seed {
struct TextureAsset {
    std::string name;
    unsigned width{}, height{};
    std::span<const std::uint8_t> blocks;
};
class Pack final {
public:
    Pack() = default; // An empty pack, for games that ship no textures.
    explicit Pack(const std::filesystem::path& file) : bytes_(read_blob(file)) {
        Reader input(bytes_);
        if (input.u32() != 0x4b504453 || input.u32() != 1) throw std::runtime_error("Unknown asset pack");
        const auto count = input.u32();
        if (count > 64) throw std::runtime_error("Asset pack entry limit exceeded");
        entries_.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            const auto length = input.u16();
            if (!length || length > 128) throw std::runtime_error("Invalid asset name length");
            const auto name_bytes = input.take(length);
            std::string name(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
            for (const auto& entry : entries_)
                if (entry.name == name) throw std::runtime_error("Duplicate asset name");
            const auto format = input.u8();
            const auto width = input.u16(), height = input.u16();
            const auto size = input.u32(), checksum = input.u32();
            if (format != 1 || !width || !height || width % 4 || height % 4 || width > 4096 ||
                height > 4096 || size != unsigned(width) * height)
                throw std::runtime_error("Invalid BC3 asset record");
            const auto blocks = input.take(size);
            if (crc32(blocks) != checksum) throw std::runtime_error("Corrupt asset block");
            entries_.push_back({std::move(name), width, height, blocks});
        }
        if (!input.done()) throw std::runtime_error("Trailing asset data");
    }
    Pack(const Pack&) = delete;
    Pack& operator=(const Pack&) = delete;
    const TextureAsset& texture(std::string_view name) const {
        for (const auto& entry : entries_)
            if (entry.name == name) return entry;
        throw std::runtime_error("Missing packed texture: " + std::string(name));
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::vector<TextureAsset> entries_;
};
class PackStream final {
public:
    PackStream(Jobs& jobs, std::filesystem::path path) : jobs_(jobs), path_(std::move(path)) {
        jobs_.submit({load, this, &group_});
    }
    ~PackStream() { jobs_.wait(group_); }
    const Pack& get() {
        jobs_.wait(group_);
        if (error_) std::rethrow_exception(error_);
        return *pack_;
    }

private:
    static void load(void* context) noexcept {
        auto& task = *static_cast<PackStream*>(context);
        try {
            task.pack_ = std::make_unique<Pack>(task.path_);
        } catch (...) {
            task.error_ = std::current_exception();
        }
    }
    Jobs& jobs_;
    JobGroup group_;
    std::filesystem::path path_;
    std::unique_ptr<Pack> pack_;
    std::exception_ptr error_;
};
} // namespace seed
