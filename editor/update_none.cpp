#include "update_platform.hpp"
#include <stdexcept>

namespace seed::editor {
// Windows and Linux updates arrive with packaged editors for those platforms; until then the
// editor reports that it cannot update itself.
namespace {
[[noreturn]] void unsupported() {
    throw std::runtime_error("This platform cannot update the editor");
}
} // namespace

bool updates_supported() {
    return false;
}
std::filesystem::path running_bundle() {
    return {};
}
BundleInfo bundle_info(const std::filesystem::path&) {
    unsupported();
}
std::string fetch_text(const std::string&, std::size_t, const std::atomic<bool>&) {
    unsupported();
}
void fetch_file(const std::string&, const std::filesystem::path&, std::uint64_t, const std::atomic<bool>&) {
    unsupported();
}
bool verify_signature(std::span<const std::uint8_t>, std::string_view, std::span<const std::uint8_t>) {
    return false;
}
std::string sha256_file(const std::filesystem::path&) {
    unsupported();
}
} // namespace seed::editor
