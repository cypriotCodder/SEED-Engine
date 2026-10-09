#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Editor updates. A release publishes a signed feed, update.json, beside a zip of Seed Editor.app:
//   {"manifest": "<manifest JSON as text>", "signature": "<hex DER ECDSA P-256 SHA-256>"}
// The signature covers the manifest text's exact bytes, which hold the release's version, notes and
// the zip's file name, size and SHA-256. The editor carries only the public key. See
// docs/releasing.md for the format and the release steps.
namespace seed::editor {
// A release version, MAJOR.MINOR.PATCH, each part 0-999999 without leading zeros.
struct Version {
    std::uint32_t major{}, minor{}, patch{};
    auto operator<=>(const Version&) const = default;
};
// Parses a version; throws for anything but the exact form above.
Version parse_version(std::string_view text);
std::string to_string(const Version& version);

struct UpdateManifest {
    Version version;
    std::string notes;    // Shown to the user before installing; plain text.
    std::string file;     // The zip's name, beside the feed.
    std::uint64_t size{}; // The zip's exact size in bytes.
    std::string sha256;   // The zip's SHA-256, 64 lowercase hex digits.
    std::string text;     // The manifest exactly as signed, kept beside a verified download.
};
constexpr std::size_t update_feed_limit = 64 * 1024;
constexpr std::uint64_t update_size_limit = std::uint64_t(1) << 30; // 1 GiB.

// Parses manifest text strictly: format 1, product "seed-editor", and exactly the fields above.
UpdateManifest parse_update_manifest(std::string_view text);
// Checks a feed's signature with `public_key`, then parses its manifest. Throws on any problem.
UpdateManifest read_update_feed(std::string_view feed, std::span<const std::uint8_t> public_key);
// Lowercase or uppercase hex to bytes; throws for odd lengths or other characters.
std::vector<std::uint8_t> from_hex(std::string_view hex);

// Where a download is fetched from: the feed URL's folder plus the manifest's file name.
std::string sibling_url(std::string_view feed_url, std::string_view file);

// Replaces the app bundle `app` with the one in `zip`, which must hold exactly `app`'s name, with
// the same bundle identifier and `version`, and a valid code signature. The new app is unpacked
// beside the old one and swapped in with renames; on failure the old app stays in place.
void install_update(const std::filesystem::path& zip, const Version& version,
                    const std::filesystem::path& app);

// Checks for, downloads and installs editor updates. Network and file work runs on one worker
// thread; the editor calls poll() each frame and reads status(). Files live in `folder`:
// settings.json (preferences and the last check), and a verified download with ready.json.
class Updater final {
public:
    struct Config {
        std::string feed_url;                 // Empty turns updates off.
        std::vector<std::uint8_t> public_key; // Empty turns updates off.
        Version current;
        std::filesystem::path folder;
        std::filesystem::path app; // The bundle to replace; empty when not running from one.
    };
    enum class State { off, idle, checking, downloading, up_to_date, ready, failed };
    struct Status {
        State state{State::off};
        bool manual{};       // The user asked for this check, so its result is worth showing.
        std::string message; // Why it failed, or why updates are off.
        std::optional<UpdateManifest> update; // The newer release, once known.
    };
    // One automatic check a day at most.
    static constexpr std::chrono::hours check_interval{24};

    // `off_reason` explains, when updates cannot run, why (no feed, no key, not a bundle).
    Updater(Config config, std::string off_reason);
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    bool enabled() const { return enabled_; }
    bool busy() const;
    Status status() const;
    // Starts a check unless one is running. A manual check also shows skipped versions.
    void check(bool manual);
    // Starts an automatic check when automatic checks are on and the last one is old enough.
    void check_if_due();
    // Finishes a completed check on the main thread. Returns what is worth logging, each line
    // flagged true when it reports a problem.
    std::vector<std::pair<bool, std::string>> poll();

    bool automatic() const { return automatic_; }
    void set_automatic(bool on);
    // Forgets the ready download and stops offering this version until a newer one appears.
    void skip();
    // Installs a verified download into Config::app. Throws on failure, leaving the old app.
    // Returns the version installed, or nothing when no update is ready.
    std::optional<Version> install();
    const std::filesystem::path& app() const { return config_.app; }

private:
    void work(bool manual, std::optional<Version> skipped);
    void save_settings() const;
    // Offers a verified download left by an earlier session, or deletes it when stale.
    void load_ready();
    // Deletes the download, its record and any partial downloads.
    void discard_downloads(const std::string& keep = {});
    void finish(State state, std::string message, std::optional<UpdateManifest> update);

    Config config_;
    bool enabled_{};
    bool automatic_{true};
    std::int64_t last_check_{}; // Seconds since the Unix epoch.
    std::optional<Version> skipped_;
    std::vector<std::pair<bool, std::string>> notes_; // Waiting for poll().
    mutable std::mutex mutex_;
    Status status_;
    bool finished_{}; // The worker has a result poll() has not taken yet.
    std::atomic<bool> cancel_{};
    std::thread worker_;
};
} // namespace seed::editor
