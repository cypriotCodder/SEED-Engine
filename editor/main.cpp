#include "app.hpp"
#include "splash.hpp"
#include <cstdio>
#include <exception>
#include <string_view>

// seed_editor [--open PROJECT] [--create PARENT NAME] [--preferences DIR] [--play] [--smoke]
//             [--splash] [--screenshot FILE.ppm] [--update-feed URL --update-key HEX] [--version]
int main(int argc, char** argv) {
    try {
        seed::editor::Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--smoke")
                options.smoke = true;
            else if (arg == "--screenshot" && i + 1 < argc)
                options.screenshot = argv[++i];
            else if (arg == "--open" && i + 1 < argc)
                options.open = argv[++i];
            else if (arg == "--create" && i + 2 < argc) {
                options.create_parent = argv[++i];
                options.create_name = argv[++i];
            } else if (arg == "--export" && i + 1 < argc)
                options.export_to = argv[++i];
            else if (arg == "--ui-test")
                options.ui_test = true;
            else if (arg == "--splash")
                options.splash = true;
            else if (arg == "--play")
                options.play = true;
            else if (arg == "--preferences" && i + 1 < argc)
                options.preferences = argv[++i];
            else if (arg == "--update-feed" && i + 1 < argc)
                options.update_feed = argv[++i];
            else if (arg == "--update-key" && i + 1 < argc)
                options.update_key = argv[++i];
            else if (arg == "--version") {
                std::printf("Seed Editor %s\n", seed::editor::editor_version);
                return 0;
            } else
                throw std::invalid_argument("Usage: seed_editor [--open PROJECT] [--create PARENT NAME] "
                                            "[--preferences DIR] [--export DIR] [--play] [--ui-test] "
                                            "[--smoke] [--splash] [--screenshot FILE.ppm] "
                                            "[--update-feed URL --update-key HEX] [--version]");
        }
        return seed::editor::App(options).run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor error: %s\n", error.what());
        return 1;
    }
}
