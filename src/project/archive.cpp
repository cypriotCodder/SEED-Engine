#include "project/archive.hpp"
#include "io/binary.hpp"
#include "io/storage.hpp"
#include <memory>

namespace seed {
namespace {
constexpr std::uint32_t archive_magic = 0x4b415053, archive_version = 1;
constexpr std::size_t file_limit = 16 * 1024 * 1024;
} // namespace

bool valid_project_path(const std::string& path) {
    if (path.empty() || path.size() > 255 || path.front() == '/' || path.back() == '/') return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = std::min(path.find('/', start), path.size());
        const auto part = std::string_view(path).substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (const char c : part)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '-' || c == '.'))
                return false;
        start = end + 1;
    }
    return true;
}

ProjectFiles folder_files(std::filesystem::path root) {
    return [root = std::move(root)](const std::string& path) -> std::optional<std::string> {
        if (!valid_project_path(path)) throw std::invalid_argument("Invalid project path: " + path);
        const auto file = root / path;
        if (!std::filesystem::is_regular_file(file)) return std::nullopt;
        return read_text(file, file_limit);
    };
}

ProjectFiles subfolder(ProjectFiles files, std::string prefix) {
    return [files = std::move(files), prefix = std::move(prefix)](const std::string& path) {
        return files(prefix + path);
    };
}

void ProjectArchive::write(const std::filesystem::path& file) const {
    Bytes out;
    out.u32(archive_magic);
    out.u32(archive_version);
    out.u32(static_cast<std::uint32_t>(files.size()));
    for (const auto& [path, bytes] : files) {
        if (!valid_project_path(path)) throw std::invalid_argument("Invalid project path: " + path);
        if (bytes.size() > file_limit) throw std::length_error("File too large to export: " + path);
        out.u16(static_cast<std::uint16_t>(path.size()));
        out.data.insert(out.data.end(), path.begin(), path.end());
        out.u32(static_cast<std::uint32_t>(bytes.size()));
        out.data.insert(out.data.end(), bytes.begin(), bytes.end());
    }
    write_blob(file, out.data);
}

ProjectArchive ProjectArchive::read(const std::filesystem::path& file) {
    const auto bytes = read_blob(file);
    Reader in(bytes);
    if (in.u32() != archive_magic) throw std::runtime_error("Not a Seed game archive");
    if (in.u32() != archive_version) throw std::runtime_error("Unsupported game archive version");
    ProjectArchive archive;
    const auto count = in.u32();
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto name = in.take(in.u16());
        std::string path(name.begin(), name.end());
        if (!valid_project_path(path)) throw std::runtime_error("Unsafe path in game archive");
        if (!archive.files.empty() && path <= archive.files.rbegin()->first)
            throw std::runtime_error("Game archive files out of order or duplicated");
        const auto data = in.take(in.u32());
        archive.files.emplace(std::move(path), std::string(data.begin(), data.end()));
    }
    if (!in.done()) throw std::runtime_error("Trailing data in game archive");
    return archive;
}

ProjectFiles ProjectArchive::reader() const {
    return [files = std::make_shared<const std::map<std::string, std::string>>(files)](
               const std::string& path) -> std::optional<std::string> {
        const auto found = files->find(path);
        if (found == files->end()) return std::nullopt;
        return found->second;
    };
}
} // namespace seed
