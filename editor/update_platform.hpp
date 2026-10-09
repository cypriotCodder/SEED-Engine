#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// The operating-system half of editor updates: fetching, checking signatures and hashes, and
// reading app bundles. macOS uses its own frameworks (update_mac.mm); elsewhere updates are off.
namespace seed::editor {
// Whether this platform can fetch, verify and install editor updates.
bool updates_supported();
// The .app bundle the editor is running from, or empty when it runs outside one (a build tree).
std::filesystem::path running_bundle();

struct BundleInfo {
    std::string identifier, version; // CFBundleIdentifier and CFBundleShortVersionString.
};
// Reads an app bundle's Info.plist; throws when it is missing or lacks either value.
BundleInfo bundle_info(const std::filesystem::path& app);

// Fetches a URL into memory, refusing more than `limit` bytes. Blocks until done; throws on
// failure, on an HTTP status other than 200, or when `cancel` becomes true.
std::string fetch_text(const std::string& url, std::size_t limit, const std::atomic<bool>& cancel);
// Downloads a URL to the file `to` (replacing it), refusing more than `limit` bytes. As fetch_text.
void fetch_file(const std::string& url, const std::filesystem::path& to, std::uint64_t limit,
                const std::atomic<bool>& cancel);

// Checks a DER-encoded ECDSA P-256 / SHA-256 signature of `message` against an uncompressed
// (65-byte X9.63) public key. False for a bad signature or an unusable key.
bool verify_signature(std::span<const std::uint8_t> public_key, std::string_view message,
                      std::span<const std::uint8_t> signature);
// The SHA-256 of a file, as 64 lowercase hex digits; throws when it cannot be read.
std::string sha256_file(const std::filesystem::path& file);
} // namespace seed::editor
