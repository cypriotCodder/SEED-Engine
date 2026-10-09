#include "update.hpp"
#include "export.hpp"
#include "io/json.hpp"
#include "io/storage.hpp"
#include "update_platform.hpp"
#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <stdexcept>

namespace seed::editor {
namespace fs = std::filesystem;
namespace {
constexpr std::string_view product = "seed-editor";
constexpr std::size_t notes_limit = 16 * 1024;
constexpr std::size_t signature_limit = 80; // DER ECDSA P-256 signatures are at most 72 bytes.
constexpr const char* ready_file = "ready.json";
constexpr const char* settings_file = "settings.json";
constexpr const char* failure_file = "install-error.txt"; // Why installing on the last quit failed.
constexpr std::chrono::hours retry_interval{1};           // After a failed automatic check.

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}
bool is_lower_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f');
}
// A name without folders that every file system accepts: letters, digits, '.', '_' and '-'.
bool plain_file_name(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.front() == '.') return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
               c == '-';
    });
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The downloaded and partly downloaded zips in the updates folder.
std::vector<fs::path> downloads(const fs::path& folder) {
    std::vector<fs::path> found;
    std::error_code ignored;
    for (const auto& entry : fs::directory_iterator(folder, ignored))
        if (entry.path().extension() == ".zip" || entry.path().extension() == ".partial")
            found.push_back(entry.path());
    return found;
}

// Requires a JSON object with exactly these members.
void exact_members(const Json& object, std::initializer_list<std::string_view> names,
                   const std::string& what) {
    if (!object.is(Json::Type::object)) throw std::runtime_error(what + " is not a JSON object");
    for (const auto& member : object.members())
        if (std::find(names.begin(), names.end(), member.first) == names.end())
            throw std::runtime_error(what + " has an unknown field \"" + member.first + "\"");
    for (const auto name : names)
        if (!object.find(name)) throw std::runtime_error(what + " is missing \"" + std::string(name) + "\"");
}
} // namespace

Version parse_version(std::string_view text) {
    Version version;
    std::uint32_t* const parts[] = {&version.major, &version.minor, &version.patch};
    std::size_t start = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto end = i < 2 ? text.find('.', start) : text.size();
        const auto part =
            end == std::string_view::npos ? std::string_view{} : text.substr(start, end - start);
        if (part.empty() || part.size() > 6 || (part.size() > 1 && part[0] == '0') ||
            !std::all_of(part.begin(), part.end(), is_digit))
            throw std::runtime_error("Not a version (MAJOR.MINOR.PATCH): \"" + std::string(text) + "\"");
        std::from_chars(part.data(), part.data() + part.size(), *parts[i]);
        start = end + 1;
    }
    return version;
}

std::string to_string(const Version& version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
           std::to_string(version.patch);
}

std::vector<std::uint8_t> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) throw std::runtime_error("Hex text has an odd number of digits");
    const auto digit = [](char c) -> std::uint8_t {
        if (is_digit(c)) return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
        throw std::runtime_error("Hex text has a character that is not a hex digit");
    };
    std::vector<std::uint8_t> bytes(hex.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(digit(hex[2 * i]) << 4 | digit(hex[2 * i + 1]));
    return bytes;
}

UpdateManifest parse_update_manifest(std::string_view text) {
    const auto json = parse_json(text);
    const std::string what = "The update manifest";
    exact_members(json, {"format", "product", "version", "notes", "file", "size", "sha256"}, what);
    if (json.at("format").as_int(0, 1000) != 1)
        throw std::runtime_error(what + " has an unknown format; this editor reads format 1");
    if (json.at("product").as_string() != product)
        throw std::runtime_error(what + " is for \"" + json.at("product").as_string() + "\", not " +
                                 std::string(product));
    UpdateManifest manifest;
    manifest.version = parse_version(json.at("version").as_string());
    manifest.notes = json.at("notes").as_string();
    if (manifest.notes.size() > notes_limit) throw std::runtime_error(what + "'s notes are over 16 KB");
    manifest.file = json.at("file").as_string();
    if (!plain_file_name(manifest.file) || !manifest.file.ends_with(".zip"))
        throw std::runtime_error(what + " names \"" + manifest.file +
                                 "\"; it must be a plain .zip file name");
    manifest.size =
        static_cast<std::uint64_t>(json.at("size").as_int(1, static_cast<std::int64_t>(update_size_limit)));
    manifest.sha256 = json.at("sha256").as_string();
    if (manifest.sha256.size() != 64 ||
        !std::all_of(manifest.sha256.begin(), manifest.sha256.end(), is_lower_hex))
        throw std::runtime_error(what + "'s sha256 is not 64 lowercase hex digits");
    manifest.text = std::string(text);
    return manifest;
}

UpdateManifest read_update_feed(std::string_view feed, std::span<const std::uint8_t> public_key) {
    if (feed.size() > update_feed_limit) throw std::runtime_error("The update feed is over 64 KB");
    const auto json = parse_json(feed);
    exact_members(json, {"manifest", "signature"}, "The update feed");
    const auto& manifest = json.at("manifest").as_string();
    const auto& signature = json.at("signature").as_string();
    if (signature.empty() || signature.size() > 2 * signature_limit)
        throw std::runtime_error("The update feed's signature has the wrong length");
    if (!verify_signature(public_key, manifest, from_hex(signature)))
        throw std::runtime_error("The update feed's signature does not match Seed's release key");
    return parse_update_manifest(manifest);
}

std::string sibling_url(std::string_view feed_url, std::string_view file) {
    const auto slash = feed_url.rfind('/');
    if (slash == std::string_view::npos || feed_url.find("://") == std::string_view::npos)
        throw std::runtime_error("The update feed address is not a URL: " + std::string(feed_url));
    return std::string(feed_url.substr(0, slash + 1)) + std::string(file);
}

void install_update(const fs::path& zip, const Version& version, const fs::path& app) {
    if (app.empty() || app.extension() != ".app")
        throw std::runtime_error(
            "Updates install only into Seed Editor.app, and this editor is not running from it");
    const auto current = bundle_info(app);
    const auto parent = app.parent_path(), name = app.filename();
    // Hidden folders beside the app, so the swap is two renames on one volume.
    const auto incoming = parent / ("." + app.stem().string() + ".update");
    const auto previous = parent / ("." + app.stem().string() + ".previous");
    std::error_code ignored;
    try {
        fs::remove_all(incoming);
        fs::create_directories(incoming);
        if (run_tool({"/usr/bin/ditto", "-x", "-k", zip.string(), incoming.string()}) != 0)
            throw std::runtime_error("Could not unpack the update");
        const auto unpacked = incoming / name;
        std::size_t entries = 0;
        for (const auto& entry : fs::directory_iterator(incoming)) {
            (void)entry;
            ++entries;
        }
        if (entries != 1 || !fs::is_directory(unpacked))
            throw std::runtime_error("The update does not hold just " + name.string());
        const auto info = bundle_info(unpacked);
        if (info.identifier != current.identifier)
            throw std::runtime_error("The update is " + info.identifier + ", not " + current.identifier);
        if (info.version != to_string(version))
            throw std::runtime_error("The update's app says version " + info.version + ", not " +
                                     to_string(version));
        if (run_tool({"/usr/bin/codesign", "--verify", "--deep", "--strict", unpacked.string()}) != 0)
            throw std::runtime_error("The update's code signature is not valid");
        fs::remove_all(previous);
        fs::rename(app, previous);
        try {
            fs::rename(unpacked, app);
        } catch (...) {
            std::error_code back;
            fs::rename(previous, app, back);
            if (back)
                throw std::runtime_error(
                    "The update failed and the previous editor could not be put back; it is at " +
                    previous.string());
            throw;
        }
    } catch (const fs::filesystem_error& error) {
        fs::remove_all(incoming, ignored);
        throw std::runtime_error(std::string("Could not install the update beside ") + app.string() + ": " +
                                 error.code().message());
    } catch (...) {
        fs::remove_all(incoming, ignored);
        throw;
    }
    fs::remove_all(previous, ignored);
    fs::remove_all(incoming, ignored);
}

Updater::Updater(Config config, std::string off_reason) : config_(std::move(config)) {
    if (off_reason.empty() && !updates_supported()) off_reason = "This platform cannot update the editor.";
    if (off_reason.empty() && (config_.feed_url.empty() || config_.public_key.empty()))
        off_reason = "This editor was built without an update feed.";
    if (!off_reason.empty()) {
        status_.message = std::move(off_reason);
        return;
    }
    enabled_ = true;
    status_.state = State::idle;
    try {
        const auto path = config_.folder / settings_file;
        if (fs::exists(path)) {
            const auto json = parse_json(read_text(path, 4096));
            exact_members(json, {"automatic", "last_check", "skipped"}, "settings.json");
            automatic_ = json.at("automatic").as_bool();
            last_check_ = json.at("last_check").as_int(0);
            const auto& skipped = json.at("skipped").as_string();
            if (!skipped.empty()) skipped_ = parse_version(skipped);
        }
    } catch (const std::exception& error) {
        notes_.emplace_back(true, std::string("Update settings were unreadable, so they are reset: ") +
                                      error.what());
    }
    load_ready();
    // Installing happens as the editor quits, so a failure is reported on the next start.
    const auto failure = config_.folder / failure_file;
    std::error_code error;
    if (fs::exists(failure, error)) {
        try {
            status_.message = "Installing the update failed: " + read_text(failure, 4096);
            notes_.emplace_back(true, status_.message);
        } catch (const std::exception&) {}
        fs::remove(failure, error);
    }
}

Updater::~Updater() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool Updater::busy() const {
    std::lock_guard lock(mutex_);
    return finished_ || status_.state == State::checking || status_.state == State::downloading;
}

Updater::Status Updater::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void Updater::check(bool manual) {
    if (!enabled_ || busy()) return;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard lock(mutex_);
        status_.state = State::checking;
        status_.manual = manual;
        status_.message.clear();
    }
    cancel_ = false;
    worker_ = std::thread([this, manual, skipped = skipped_] { work(manual, skipped); });
}

void Updater::check_if_due() {
    if (enabled_ && automatic_ && !busy() &&
        now_seconds() - last_check_ >= std::chrono::seconds(check_interval).count())
        check(false);
}

void Updater::work(bool manual, std::optional<Version> skipped) {
    try {
        auto manifest =
            read_update_feed(fetch_text(config_.feed_url, update_feed_limit, cancel_), config_.public_key);
        if (manifest.version <= config_.current || (!manual && skipped && manifest.version <= *skipped))
            return finish(State::up_to_date, {}, std::nullopt);
        {
            std::lock_guard lock(mutex_);
            status_.state = State::downloading;
            status_.update = manifest;
        }
        const auto zip = config_.folder / manifest.file;
        // An earlier session may have fetched it already.
        std::error_code missing;
        const bool have =
            fs::file_size(zip, missing) == manifest.size && !missing && sha256_file(zip) == manifest.sha256;
        if (have)
            discard_downloads(manifest.file);
        else {
            fs::create_directories(config_.folder);
            discard_downloads();
            auto partial = zip;
            partial += ".partial";
            fetch_file(sibling_url(config_.feed_url, manifest.file), partial, manifest.size, cancel_);
            if (const auto size = fs::file_size(partial); size != manifest.size)
                throw std::runtime_error("The download is " + std::to_string(size) +
                                         " bytes; the signed feed says " + std::to_string(manifest.size));
            if (sha256_file(partial) != manifest.sha256)
                throw std::runtime_error("The download's SHA-256 does not match the signed feed");
            fs::rename(partial, zip);
        }
        write_text(config_.folder / ready_file, manifest.text);
        finish(State::ready, {}, std::move(manifest));
    } catch (const std::exception& error) {
        std::error_code ignored;
        for (const auto& file : downloads(config_.folder))
            if (file.extension() == ".partial") fs::remove(file, ignored);
        finish(State::failed, error.what(), std::nullopt);
    } catch (...) {
        finish(State::failed, "Unknown failure", std::nullopt);
    }
}

void Updater::finish(State state, std::string message, std::optional<UpdateManifest> update) {
    std::lock_guard lock(mutex_);
    status_.state = state;
    status_.message = std::move(message);
    status_.update = std::move(update);
    finished_ = true;
}

std::vector<std::pair<bool, std::string>> Updater::poll() {
    auto out = std::move(notes_);
    notes_.clear();
    Status done;
    {
        std::lock_guard lock(mutex_);
        if (!finished_) return out;
        finished_ = false;
        done = status_;
    }
    worker_.join();
    last_check_ = now_seconds();
    if (done.state == State::failed)
        last_check_ -= std::chrono::seconds(check_interval - retry_interval).count(); // Try again sooner.
    try {
        save_settings();
    } catch (const std::exception& error) {
        out.emplace_back(true, std::string("Could not save update settings: ") + error.what());
    }
    if (done.state == State::up_to_date && done.manual)
        out.emplace_back(false, "Seed Editor " + to_string(config_.current) + " is up to date.");
    else if (done.state == State::ready)
        out.emplace_back(false, "Seed Editor " + to_string(done.update->version) +
                                    " is downloaded and verified; it installs when you quit.");
    else if (done.state == State::failed) {
        out.emplace_back(done.manual, "Could not update Seed Editor: " + done.message);
        load_ready(); // A download from an earlier check is still good.
    }
    return out;
}

void Updater::set_automatic(bool on) {
    automatic_ = on;
    save_settings();
}

void Updater::skip() {
    Status current = status();
    if (current.state != State::ready) return;
    skipped_ = current.update->version;
    discard_downloads();
    {
        std::lock_guard lock(mutex_);
        status_.state = State::idle;
        status_.update.reset();
    }
    save_settings();
}

std::optional<Version> Updater::install() {
    if (!enabled_) return std::nullopt;
    // A check still running at quit is abandoned; the download it replaces stays usable.
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard lock(mutex_);
        finished_ = false;
        status_.state = State::idle;
        status_.update.reset();
    }
    load_ready();
    const auto ready = status();
    if (ready.state != State::ready) return std::nullopt;
    const auto& manifest = *ready.update;
    const auto zip = config_.folder / manifest.file;
    if (sha256_file(zip) != manifest.sha256) {
        discard_downloads();
        throw std::runtime_error("The downloaded update changed on disk; it will be downloaded again");
    }
    try {
        install_update(zip, manifest.version, config_.app);
    } catch (const std::exception& error) {
        try {
            write_text(config_.folder / failure_file, error.what());
        } catch (const std::exception&) {}
        throw;
    }
    discard_downloads();
    return manifest.version;
}

void Updater::save_settings() const {
    auto json = Json::object();
    json.set("automatic", automatic_);
    json.set("last_check", last_check_);
    json.set("skipped", skipped_ ? to_string(*skipped_) : std::string());
    fs::create_directories(config_.folder);
    write_text(config_.folder / settings_file, to_json(json));
}

void Updater::load_ready() {
    const auto path = config_.folder / ready_file;
    std::error_code error;
    if (!fs::exists(path, error)) return;
    try {
        auto manifest = parse_update_manifest(read_text(path, update_feed_limit));
        const auto zip = config_.folder / manifest.file;
        // Its hash is checked again before installing; here a size check is enough.
        if (manifest.version > config_.current && (!skipped_ || manifest.version > *skipped_) &&
            fs::file_size(zip, error) == manifest.size && !error) {
            std::lock_guard lock(mutex_);
            status_.state = State::ready;
            status_.update = std::move(manifest);
            return;
        }
    } catch (const std::exception& problem) {
        notes_.emplace_back(true, std::string("A downloaded update was unreadable, so it is deleted: ") +
                                      problem.what());
    }
    discard_downloads();
}

void Updater::discard_downloads(const std::string& keep) {
    std::error_code ignored;
    fs::remove(config_.folder / ready_file, ignored);
    for (const auto& file : downloads(config_.folder))
        if (file.filename() != keep) fs::remove(file, ignored);
}
} // namespace seed::editor
