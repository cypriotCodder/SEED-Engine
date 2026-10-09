#include "export.hpp"
#include "io/json.hpp"
#include "io/storage.hpp"
#include "update.hpp"
#include "update_platform.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

// seed_editor_update_tests FIXTURES SCRATCH
// FIXTURES is editor/tests/update: a feed signed with its test-only key (see its README).
namespace {
namespace fs = std::filesystem;
using namespace seed;
using namespace seed::editor;

constexpr const char* test_key = "04d1902a9ce5970d41cca0f0095758f327b47546ef3a5679efa846fe43776046d254af9a03a"
                                 "10ec0e16a920eb0333e1c3bfc984606b"
                                 "9710eb2b92d8e3cd3251142";
constexpr const char* other_key = // A valid key that did not sign the fixtures.
    "040d9aba04203cdbf5a708b9577b05c70e51aa676104758ffa6c12ce78db232029869983731c0b62de1e91bd36c792a123c0f478"
    "fe0b"
    "14dd94201941628ea9b2cd";

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F>
std::string rejects(F&& f, const std::string& message) {
    try {
        f();
    } catch (const std::exception& error) {
        return error.what();
    }
    throw std::runtime_error("Accepted: " + message);
}

// A file:// URL, with everything but unreserved characters and '/' percent-encoded.
std::string file_url(const fs::path& path) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string url = "file://";
    for (const unsigned char c : fs::absolute(path).string()) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '.' || c == '_' || c == '~' || c == '/')
            url += static_cast<char>(c);
        else {
            url += '%';
            url += hex[c >> 4];
            url += hex[c & 15];
        }
    }
    return url;
}

void versions() {
    check(parse_version("0.1.0") == Version{0, 1, 0}, "0.1.0");
    check(parse_version("10.20.999999") == Version{10, 20, 999999}, "Large parts");
    check(parse_version("0.1.0") < parse_version("0.1.1") &&
              parse_version("0.9.9") < parse_version("0.10.0") &&
              parse_version("1.0.0") > parse_version("0.99.99"),
          "Versions order numerically, part by part");
    check(to_string(parse_version("3.0.12")) == "3.0.12", "Version text round trip");
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.x", "-1.2.3", "1.2.3 ",
                            " 1.2.3", "1234567.0.0", "1..3", "1.2.", "v1.2.3", "1.2.3-beta"})
        rejects([&] { parse_version(bad); }, std::string("Version \"") + bad + "\"");
}

void hex() {
    check(from_hex("00ff7A") == std::vector<std::uint8_t>{0x00, 0xff, 0x7a}, "Hex decodes either case");
    rejects([] { from_hex("abc"); }, "Odd hex length");
    rejects([] { from_hex("zz"); }, "Non-hex digits");
}

Json manifest_json() {
    auto json = Json::object();
    json.set("format", 1);
    json.set("product", "seed-editor");
    json.set("version", "0.2.0");
    json.set("notes", "Faster terrain painting.");
    json.set("file", "Seed-Editor-0.2.0-macos.zip");
    json.set("size", 1234);
    json.set("sha256", std::string(64, 'a'));
    return json;
}

void manifests() {
    const auto manifest = parse_update_manifest(to_json(manifest_json()));
    check(manifest.version == Version{0, 2, 0} && manifest.notes == "Faster terrain painting." &&
              manifest.file == "Seed-Editor-0.2.0-macos.zip" && manifest.size == 1234 &&
              manifest.sha256 == std::string(64, 'a') && manifest.text == to_json(manifest_json()),
          "A valid manifest reads back");
    const auto changed = [](const char* key, Json value) {
        auto json = manifest_json();
        json.set(key, std::move(value));
        return to_json(json);
    };
    const std::pair<std::string, const char*> bad[] = {
        {changed("format", 2), "a later format"},
        {changed("product", "seed-player"), "another product"},
        {changed("version", "0.2"), "a short version"},
        {changed("file", "../Seed.zip"), "a path"},
        {changed("file", "sub/Seed.zip"), "a folder"},
        {changed("file", ".Seed.zip"), "a hidden file"},
        {changed("file", "Seed.tar"), "not a zip"},
        {changed("size", 0), "an empty download"},
        {changed("size", std::int64_t(1) << 31), "a download over 1 GiB"},
        {changed("size", 12.5), "a fractional size"},
        {changed("sha256", std::string(64, 'A')), "uppercase hash"},
        {changed("sha256", std::string(63, 'a')), "a short hash"},
        {changed("notes", std::string(16 * 1024 + 1, 'n')), "notes over 16 KB"},
        {changed("extra", true), "an unknown field"},
        {"[]", "not an object"},
        {to_json(manifest_json()) + "x", "trailing text"},
    };
    for (const auto& [text, what] : bad)
        rejects([&] { parse_update_manifest(text); }, std::string("Manifest with ") + what);
    const auto missing = manifest_json();
    auto members = Json::object();
    for (const auto& [key, value] : missing.members())
        if (key != "sha256") members.set(key, value);
    rejects([&] { parse_update_manifest(to_json(members)); }, "Manifest without sha256");
}

void urls() {
    check(sibling_url("https://example.com/r/latest/download/update.json", "Seed.zip") ==
              "https://example.com/r/latest/download/Seed.zip",
          "Downloads sit beside the feed");
    check(sibling_url("file:///a%20b/update.json", "x.zip") == "file:///a%20b/x.zip", "File URLs too");
    rejects([] { sibling_url("update.json", "x.zip"); }, "A feed address that is not a URL");
}

void feeds(const fs::path& fixtures) {
    const auto key = from_hex(test_key);
    const auto feed = read_text(fixtures / "feed.json");
    const auto manifest = read_update_feed(feed, key);
    check(manifest.version == Version{9, 0, 0} && manifest.notes == "Test release notes.\nSecond line." &&
              manifest.size == fs::file_size(fixtures / manifest.file) &&
              manifest.sha256 == sha256_file(fixtures / manifest.file),
          "The signed fixture feed reads and matches its zip");

    const auto message = rejects([&] { read_update_feed(feed, from_hex(other_key)); }, "Another key's feed");
    check(message.find("does not match Seed's release key") != std::string::npos,
          "Wrong key explained: " + message);
    rejects([&] { read_update_feed(feed, std::vector<std::uint8_t>(65, 4)); }, "An unusable key");

    const auto json = parse_json(feed);
    const auto with = [&](const std::string& manifest_text, const std::string& signature) {
        auto changed = Json::object();
        changed.set("manifest", manifest_text);
        changed.set("signature", signature);
        return to_json(changed);
    };
    const auto& text = json.at("manifest").as_string();
    const auto& signature = json.at("signature").as_string();
    check(read_update_feed(with(text, signature), key).version == Version{9, 0, 0},
          "Re-encoding keeps the bytes");
    auto altered = text;
    altered.replace(altered.find("9.0.0"), 5, "9.0.1");
    rejects([&] { read_update_feed(with(altered, signature), key); }, "A manifest changed after signing");
    auto flipped = signature;
    flipped.back() = flipped.back() == '0' ? '1' : '0';
    rejects([&] { read_update_feed(with(text, flipped), key); }, "A changed signature");
    rejects([&] { read_update_feed(with(text, signature.substr(1)), key); }, "An odd-length signature");
    rejects([&] { read_update_feed(with(text, ""), key); }, "No signature");
    rejects([&] { read_update_feed(with(text, std::string(162, 'a')), key); }, "An over-long signature");
    auto extra = json;
    extra.set("url", "https://example.com/other.zip");
    rejects([&] { read_update_feed(to_json(extra), key); }, "A feed with an unsigned extra field");
    rejects([&] { read_update_feed(std::string(update_feed_limit + 1, ' '), key); }, "A feed over 64 KB");
}

void finish(Updater& updater) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (updater.busy()) {
        updater.poll();
        check(std::chrono::steady_clock::now() < deadline, "The update check finished in 30 seconds");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

Updater::Config config(const fs::path& feed, const fs::path& folder, const char* current = "0.1.0") {
    return {file_url(feed), from_hex(test_key), parse_version(current), folder, {}};
}

void updater(const fs::path& fixtures, const fs::path& scratch) {
    const auto folder = scratch / "updates";
    const auto zip = folder / "Seed-Editor-9.0.0-macos.zip";
    {
        Updater updater(config(fixtures / "feed.json", folder), {});
        check(updater.enabled() && updater.status().state == Updater::State::idle,
              "Configured updates start idle");
        updater.check(true);
        finish(updater);
        const auto status = updater.status();
        check(status.state == Updater::State::ready && status.update &&
                  status.update->version == Version{9, 0, 0},
              "A newer release downloads: " + status.message);
        check(fs::is_regular_file(zip) && sha256_file(zip) == sha256_file(fixtures / zip.filename()) &&
                  fs::is_regular_file(folder / "ready.json"),
              "The verified download and its record are kept");
        // The last check is remembered, so no automatic check is due again yet.
        updater.check_if_due();
        check(!updater.busy(), "Automatic checks wait a day after a check");
    }
    {
        Updater again(config(fixtures / "feed.json", folder), {});
        check(again.status().state == Updater::State::ready, "The next start offers the download");
        // Installing needs the editor's app bundle; without one the download stays and the failure
        // is reported on the next start.
        const auto message = rejects([&] { again.install(); }, "Installing outside an app bundle");
        check(message.find("Seed Editor.app") != std::string::npos, "Install failure explained: " + message);
    }
    {
        Updater reported(config(fixtures / "feed.json", folder), {});
        const auto notes = reported.poll();
        check(notes.size() == 1 && notes[0].first &&
                  notes[0].second.find("Installing the update failed") == 0,
              "The install failure is reported once on the next start");
        check(reported.status().state == Updater::State::ready, "The download is still offered");
        check(reported.poll().empty() && !fs::exists(folder / "install-error.txt"), "Reported only once");
    }
    {
        // Once this version is installed, the leftover download is deleted.
        Updater current(config(fixtures / "feed.json", folder, "9.0.0"), {});
        check(current.status().state == Updater::State::idle && !fs::exists(zip) &&
                  !fs::exists(folder / "ready.json"),
              "A download no newer than the editor is deleted");
        current.check(true);
        finish(current);
        check(current.status().state == Updater::State::up_to_date, "The same version is up to date");
    }
    {
        Updater skipping(config(fixtures / "feed.json", folder), {});
        skipping.check(true);
        finish(skipping);
        skipping.skip();
        check(skipping.status().state == Updater::State::idle && !fs::exists(zip),
              "Skipping deletes the download");
        skipping.check(false);
        finish(skipping);
        check(skipping.status().state == Updater::State::up_to_date,
              "Automatic checks pass over a skipped version");
        skipping.check(true);
        finish(skipping);
        check(skipping.status().state == Updater::State::ready, "Asking explicitly still finds it");
        skipping.set_automatic(false);
    }
    {
        Updater settings(config(fixtures / "feed.json", folder), {});
        check(!settings.automatic(), "Turning automatic checks off is remembered");
        const auto saved = parse_json(read_text(folder / "settings.json"));
        check(saved.at("skipped").as_string() == "9.0.0", "The skipped version is remembered");
    }

    const auto tampered_folder = scratch / "tampered";
    Updater tampered(config(fixtures / "tampered" / "feed.json", tampered_folder), {});
    tampered.check(true);
    finish(tampered);
    const auto status = tampered.status();
    check(status.state == Updater::State::failed &&
              status.message.find("does not match the signed feed") != std::string::npos,
          "A download that differs from the signed feed is refused: " + status.message);
    for (const auto& entry : fs::directory_iterator(tampered_folder))
        check(entry.path().filename() == "settings.json",
              "Nothing refused is kept: " + entry.path().string());

    Updater missing(config(fixtures / "no-such-feed.json", scratch / "missing"), {});
    missing.check(true);
    finish(missing);
    check(missing.status().state == Updater::State::failed, "A missing feed fails the check");

    Updater off({file_url(fixtures / "feed.json"), {}, {0, 1, 0}, scratch / "off", {}}, {});
    off.check(true);
    check(!off.enabled() && !off.busy() && off.status().state == Updater::State::off &&
              !off.status().message.empty(),
          "Without a key, updates are off and say why");
    Updater dev(config(fixtures / "feed.json", scratch / "dev"), "Not a bundle.");
    check(!dev.enabled() && dev.status().message == "Not a bundle.", "A reason given turns updates off");
}

// Writes a minimal app bundle: an Info.plist and one file.
void fake_app(const fs::path& app, const char* identifier, const char* version) {
    fs::create_directories(app / "Contents");
    write_text(app / "Contents" / "Info.plist",
               std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>)") +
                   identifier + "</string>\n<key>CFBundleShortVersionString</key><string>" + version +
                   "</string>\n</dict></plist>\n");
    write_text(app / "Contents" / "marker", version);
}

// The checks before an update replaces the app; each refusal leaves the installed app untouched.
// (tests in editor/CMakeLists.txt install a real Seed Editor.app.)
void installs(const fs::path& scratch) {
    const auto installed = scratch / "Applications" / "Seed Editor.app";
    fake_app(installed, "games.seed.editor", "0.1.0");
    const auto zip_of = [&](const char* name, const char* identifier, const char* version, bool extra) {
        const auto folder = scratch / "release" / name;
        fake_app(folder / "Seed Editor.app", identifier, version);
        if (extra) write_text(folder / "README.txt", "extra");
        const auto zip = scratch / "release" / (std::string(name) + ".zip");
        check(run_tool({"/usr/bin/ditto", "-c", "-k", folder.string(), zip.string()}) == 0, "ditto zips");
        return zip;
    };
    const std::tuple<const char*, const char*, const char*, bool, const char*> bad[] = {
        {"other", "games.seed.other", "0.2.0", false, "is games.seed.other"},
        {"version", "games.seed.editor", "0.3.0", false, "says version 0.3.0"},
        {"extra", "games.seed.editor", "0.2.0", true, "does not hold just"},
        {"unsigned", "games.seed.editor", "0.2.0", false, "code signature"},
    };
    for (const auto& [name, identifier, version, extra, expected] : bad) {
        const auto zip = zip_of(name, identifier, version, extra);
        const auto message = rejects([&] { install_update(zip, {0, 2, 0}, installed); }, name);
        check(message.find(expected) != std::string::npos, std::string(name) + " refused: " + message);
        check(read_text(installed / "Contents" / "marker") == "0.1.0" &&
                  !fs::exists(scratch / "Applications" / ".Seed Editor.update"),
              std::string(name) + " leaves the installed app and no leftovers");
    }
    rejects([&] { install_update(scratch / "release" / "other.zip", {0, 2, 0}, scratch / "Seed Editor"); },
            "Installing into something that is not an app");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Usage: seed_editor_update_tests FIXTURES SCRATCH");
        const fs::path fixtures = argv[1], scratch = argv[2];
        fs::remove_all(scratch);
        fs::create_directories(scratch);
        versions();
        hex();
        manifests();
        urls();
        if (updates_supported()) {
            feeds(fixtures);
            updater(fixtures, scratch);
            installs(scratch);
        } else
            std::cout << "This platform cannot update the editor; signature and download checks skipped.\n";
        std::cout << "Editor update checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Editor update test failed: " << error.what() << '\n';
        return 1;
    }
}
