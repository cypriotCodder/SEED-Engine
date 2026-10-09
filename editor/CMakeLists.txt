# The Seed editor: a separate application for creating and building game projects. Games never link
# it or Dear ImGui.
# The licences a shipped game must carry, embedded so the editor can export them from anywhere.
set(seed_licenses_source "${CMAKE_CURRENT_BINARY_DIR}/game_licenses.cpp")
set(seed_licenses_body "namespace seed::editor {\nextern const char* const game_licenses[][2] = {\n")
foreach(seed_license SDL2 LZ4 Lua stb_vorbis)
    file(READ "${CMAKE_SOURCE_DIR}/licenses/${seed_license}.txt" seed_license_text)
    string(APPEND seed_licenses_body "    {\"${seed_license}.txt\", R\"SEEDLICENSE(${seed_license_text})SEEDLICENSE\"},\n")
endforeach()
string(APPEND seed_licenses_body "    {nullptr, nullptr}};\n} // namespace seed::editor\n")
file(GENERATE OUTPUT "${seed_licenses_source}" CONTENT "${seed_licenses_body}")
# The editor's own images (splash and icon), embedded so the editor needs no files beside it.
set(seed_brand_source "${CMAKE_CURRENT_BINARY_DIR}/brand_images.cpp")
set(seed_brand_body "#include <cstddef>\nnamespace seed::editor {\n")
foreach(seed_image splash icon)
    set(seed_image_file "${CMAKE_CURRENT_SOURCE_DIR}/resources/${seed_image}.jpg")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${seed_image_file}")
    file(READ "${seed_image_file}" seed_image_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," seed_image_bytes "${seed_image_hex}")
    file(SIZE "${seed_image_file}" seed_image_size)
    string(APPEND seed_brand_body "extern const unsigned char ${seed_image}_jpg[] = {${seed_image_bytes}};\n"
        "extern const std::size_t ${seed_image}_jpg_size = ${seed_image_size};\n")
endforeach()
string(APPEND seed_brand_body "} // namespace seed::editor\n")
file(GENERATE OUTPUT "${seed_brand_source}" CONTENT "${seed_brand_body}")
# Embed the interface font and its licence; installed editors need no system font or extra asset.
set(seed_font_source "${CMAKE_CURRENT_BINARY_DIR}/interface_font.cpp")
set(seed_font_file "${CMAKE_CURRENT_SOURCE_DIR}/resources/Karla-Regular.ttf")
set(seed_font_license "${CMAKE_SOURCE_DIR}/licenses/Karla.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${seed_font_file}" "${seed_font_license}")
file(READ "${seed_font_file}" seed_font_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," seed_font_bytes "${seed_font_hex}")
file(SIZE "${seed_font_file}" seed_font_size)
file(READ "${seed_font_license}" seed_font_notice)
file(GENERATE OUTPUT "${seed_font_source}" CONTENT
    "namespace seed::editor {\nextern const unsigned char interface_font[] = {${seed_font_bytes}};\nextern const int interface_font_size = ${seed_font_size};\nextern const char interface_font_license[] = R\"SEEDFONT(${seed_font_notice})SEEDFONT\";\n}\n")
# stb_image decodes imported images; it builds on its own, without the strict warning flags.
add_library(seed_stb STATIC stb_image.c)
target_include_directories(seed_stb SYSTEM PUBLIC "${stb_SOURCE_DIR}")
# Editor updates: the signed feed the editor checks for new releases, and the public key that feed
# must be signed with (hex, uncompressed P-256). Either empty turns update checks off. See
# docs/releasing.md for making the key and publishing releases.
set(SEED_UPDATE_FEED "https://github.com/cypriotCodder/seed-releases/releases/latest/download/update.json"
    CACHE STRING "The editor's update feed URL; empty turns updates off")
set(SEED_UPDATE_PUBLIC_KEY "043c16b9a7d260954bf649ad074f27354208177530f00675d88ec59490ab2db7ce7cf539b3010875f4b1c498275a4705e04acc5b7af2cc7d085ee6ea350c3539e3"
    CACHE STRING "Hex public key the update feed is signed with; empty turns updates off")
if(NOT SEED_UPDATE_PUBLIC_KEY STREQUAL "" AND NOT SEED_UPDATE_PUBLIC_KEY MATCHES "^04[0-9a-fA-F]+$")
    message(FATAL_ERROR "SEED_UPDATE_PUBLIC_KEY must be the hex key printed by tools/release_editor.py keygen")
endif()
string(LENGTH "${SEED_UPDATE_PUBLIC_KEY}" seed_update_key_length)
if(NOT seed_update_key_length EQUAL 0 AND NOT seed_update_key_length EQUAL 130)
    message(FATAL_ERROR "SEED_UPDATE_PUBLIC_KEY must be 130 hex digits (an uncompressed P-256 key)")
endif()

if(APPLE)
    set(seed_update_platform update_mac.mm)
else()
    set(seed_update_platform update_none.cpp)
endif()
add_library(seed_editor_core STATIC project.cpp starter.cpp scripts.cpp export.cpp textures.cpp audio_files.cpp
    update.cpp ${seed_update_platform} "${seed_licenses_source}")
seed_strict(seed_editor_core)
target_include_directories(seed_editor_core PUBLIC .)
target_link_libraries(seed_editor_core PUBLIC seed_engine PRIVATE seed_lua seed_stb)
# One version for the launch screen, About, the app bundle and update checks.
target_compile_definitions(seed_editor_core PUBLIC SEED_EDITOR_VERSION="${PROJECT_VERSION}")
if(APPLE)
    target_link_libraries(seed_editor_core PUBLIC "-framework Foundation" "-framework Security")
    # Compiled as C++ with an Objective-C++ flag, so the project needs no OBJCXX language.
    set_source_files_properties(update_mac.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
endif()

if(APPLE)
    set(seed_folder_dialog folder_dialog_mac.mm)
else()
    set(seed_folder_dialog folder_dialog_none.cpp)
endif()
add_executable(seed_editor main.cpp app.cpp asset_panels.cpp terrain_panel.cpp scene_editor.cpp play.cpp ui_test.cpp
    splash.cpp "${seed_brand_source}" "${seed_font_source}" ${seed_folder_dialog})
seed_strict(seed_editor)
target_link_libraries(seed_editor PRIVATE seed_editor_core seed_imgui)
add_dependencies(seed_editor seed_player)
target_compile_definitions(seed_editor PRIVATE SEED_PLAYER_PATH="$<TARGET_FILE:seed_player>"
    SEED_UPDATE_FEED="${SEED_UPDATE_FEED}" SEED_UPDATE_PUBLIC_KEY="${SEED_UPDATE_PUBLIC_KEY}")
if(APPLE)
    target_link_libraries(seed_editor PRIVATE "-framework AppKit" "-framework UniformTypeIdentifiers")
    # Compiled as C++ with an Objective-C++ flag, so the project needs no OBJCXX language.
    set_source_files_properties(folder_dialog_mac.mm PROPERTIES LANGUAGE CXX
        COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
endif()
install(TARGETS seed_editor RUNTIME DESTINATION bin)
if(APPLE)
    # Seed Editor.app: the editor and player in a signed bundle, the form releases and updates use.
    set(seed_editor_bundle "${CMAKE_CURRENT_BINARY_DIR}/Seed Editor.app")
    # Followed by -DVERSION and -DBUNDLE, then -P and the script.
    set(seed_editor_bundle_command ${CMAKE_COMMAND} -DEDITOR=$<TARGET_FILE:seed_editor>
        -DPLAYER=$<TARGET_FILE:seed_player> -DICON=${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.jpg)
    set(seed_editor_bundle_script "${CMAKE_SOURCE_DIR}/cmake/editor_bundle.cmake")
    add_custom_command(OUTPUT "${seed_editor_bundle}/Contents/Info.plist"
        COMMAND ${seed_editor_bundle_command} -DVERSION=${PROJECT_VERSION} "-DBUNDLE=${seed_editor_bundle}"
            -P "${seed_editor_bundle_script}"
        DEPENDS seed_editor seed_player "${seed_editor_bundle_script}" VERBATIM)
    add_custom_target(seed_editor_app ALL DEPENDS "${seed_editor_bundle}/Contents/Info.plist")
endif()

if(BUILD_TESTING)
    add_executable(seed_editor_project_tests tests/project_tests.cpp)
    seed_strict(seed_editor_project_tests)
    target_link_libraries(seed_editor_project_tests PRIVATE seed_editor_core)
    add_dependencies(seed_editor_project_tests seed_player)
    target_compile_definitions(seed_editor_project_tests PRIVATE SEED_PLAYER_PATH="$<TARGET_FILE:seed_player>"
        SEED_SAMPLE_PROJECT="${CMAKE_SOURCE_DIR}/tests/sample_project"
        SEED_AUDIO_FIXTURES="${CMAKE_SOURCE_DIR}/tests/audio")
    add_executable(seed_editor_update_tests tests/update_tests.cpp)
    seed_strict(seed_editor_update_tests)
    target_link_libraries(seed_editor_update_tests PRIVATE seed_editor_core)
    add_test(NAME editor_updates COMMAND seed_editor_update_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/update"
        "${CMAKE_CURRENT_BINARY_DIR}/update-test-data")
    add_executable(seed_editor_history_tests tests/history_tests.cpp)
    seed_strict(seed_editor_history_tests)
    target_include_directories(seed_editor_history_tests PRIVATE .)
    add_executable(seed_editor_interaction_tests tests/interaction_tests.cpp)
    seed_strict(seed_editor_interaction_tests)
    target_link_libraries(seed_editor_interaction_tests PRIVATE seed_editor_core)
    add_test(NAME editor_interactions COMMAND seed_editor_interaction_tests)
    add_test(NAME editor_history COMMAND seed_editor_history_tests)
    add_test(NAME editor_projects COMMAND seed_editor_project_tests "${CMAKE_CURRENT_BINARY_DIR}/project-test-data")
    # Creates a project through the editor, then reopens it, drawing 60 frames each time.
    set(seed_editor_smoke "${CMAKE_CURRENT_BINARY_DIR}/editor-smoke")
    add_test(NAME editor_smoke_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${seed_editor_smoke}")
    # The launch screen: decoded from the embedded artwork and drawn over the hub.
    add_test(NAME editor_splash COMMAND seed_editor --smoke --splash --preferences "${seed_editor_smoke}/splash-preferences")
    set_tests_properties(editor_splash PROPERTIES PASS_REGULAR_EXPRESSION "Launch screen done; the window is 1280 x 720")
    add_test(NAME editor_smoke_create COMMAND seed_editor --smoke --preferences "${seed_editor_smoke}/preferences"
        --create "${seed_editor_smoke}" "Smoke Test")
    add_test(NAME editor_smoke_open COMMAND seed_editor --smoke --preferences "${seed_editor_smoke}/preferences"
        --open "${seed_editor_smoke}/Smoke Test")
    set_tests_properties(editor_smoke_create PROPERTIES DEPENDS editor_smoke_clean)
    set_tests_properties(editor_smoke_open PROPERTIES DEPENDS editor_smoke_create)
    # Presses Play on the sample project; passes only if the game it launches runs and exits cleanly.
    # A copy, so the editor's layout and Play saves stay out of the source tree.
    add_test(NAME editor_play_copy COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/tests/sample_project" "${seed_editor_smoke}/Sample")
    add_test(NAME editor_play COMMAND seed_editor --smoke --play --preferences "${seed_editor_smoke}/preferences"
        --open "${seed_editor_smoke}/Sample")
    set_tests_properties(editor_play_copy PROPERTIES DEPENDS editor_smoke_open)
    set_tests_properties(editor_play PROPERTIES DEPENDS editor_play_copy)
    # Exports the copied sample project through the editor, then runs the exported app itself: the
    # player must find and load its game.seedpack from the bundle.
    set(seed_exported "${seed_editor_smoke}/Export/Sample Island.app/Contents/MacOS/Sample Island")
    add_test(NAME editor_export COMMAND seed_editor --smoke --preferences "${seed_editor_smoke}/preferences"
        --open "${seed_editor_smoke}/Sample" --export "${seed_editor_smoke}/Export")
    add_test(NAME exported_game COMMAND "${seed_exported}" --smoke --save "${seed_editor_smoke}/exported-save")
    set_tests_properties(editor_export PROPERTIES DEPENDS editor_play TIMEOUT 120)
    set_tests_properties(exported_game PROPERTIES DEPENDS editor_export TIMEOUT 120)
    # The sample game, played: a copy is opened in the editor (which packs its textures) and
    # exported, then replays play it through the player and the exported app. A new game takes
    # coins, gets bitten and finds the gem; continuing that save wins; another new game is lost.
    set(seed_coin "${seed_editor_smoke}/coin_island")
    set(seed_coin_play $<TARGET_FILE:seed_player> --project "${seed_coin}/Coin Island" --smoke --replay)
    set(seed_coin_replays "${CMAKE_SOURCE_DIR}/tests/coin_island")
    add_test(NAME coin_island_copy COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/samples/coin_island" "${seed_coin}/Coin Island")
    add_test(NAME coin_island_editor COMMAND seed_editor --smoke --preferences "${seed_coin}/preferences"
        --open "${seed_coin}/Coin Island" --export "${seed_coin}/Export")
    add_test(NAME coin_island_new_game COMMAND ${seed_coin_play} "${seed_coin_replays}/new_game.replay"
        --save "${seed_coin}/save")
    add_test(NAME coin_island_continue COMMAND ${seed_coin_play} "${seed_coin_replays}/continue.replay"
        --save "${seed_coin}/save")
    add_test(NAME coin_island_game_over COMMAND ${seed_coin_play} "${seed_coin_replays}/game_over.replay"
        --save "${seed_coin}/lost-save")
    add_test(NAME coin_island_exported COMMAND "${seed_coin}/Export/Coin Island.app/Contents/MacOS/Coin Island"
        --smoke --replay "${seed_coin_replays}/new_game.replay" --save "${seed_coin}/exported-save")
    set(seed_coin_new_game "Title: no saved game.*Entered the island.*Coin 3/5.*Ouch! Health 2.*Entered the cave.*Found the gem!")
    set_tests_properties(coin_island_copy PROPERTIES DEPENDS editor_smoke_clean)
    set_tests_properties(coin_island_editor PROPERTIES DEPENDS coin_island_copy TIMEOUT 120)
    set_tests_properties(coin_island_new_game PROPERTIES DEPENDS coin_island_editor
        PASS_REGULAR_EXPRESSION "${seed_coin_new_game}" FAIL_REGULAR_EXPRESSION "Script error|Engine error")
    set_tests_properties(coin_island_continue PROPERTIES DEPENDS coin_island_new_game
        PASS_REGULAR_EXPRESSION "Title: a saved game in the cave, 3 coins, health 2.*Entered the cave.*Entered the island.*Coin 5/5.*You win!.*Title: no saved game."
        FAIL_REGULAR_EXPRESSION "Script error|Engine error")
    set_tests_properties(coin_island_game_over PROPERTIES DEPENDS coin_island_editor
        PASS_REGULAR_EXPRESSION "Health 0.*Game over.*Title: no saved game." FAIL_REGULAR_EXPRESSION "Script error|Engine error")
    set_tests_properties(coin_island_exported PROPERTIES DEPENDS coin_island_editor TIMEOUT 120
        PASS_REGULAR_EXPRESSION "${seed_coin_new_game}" FAIL_REGULAR_EXPRESSION "Script error|Engine error")
    # Updating a real Seed Editor.app: an app marked 99.0.0 is packed and its feed signed with the
    # test-only key in tests/update, then a copy of this build's app checks that feed, downloads and
    # verifies the update, and installs it as it quits.
    find_package(Python3 COMPONENTS Interpreter)
    find_program(SEED_OPENSSL openssl)
    if(APPLE AND Python3_Interpreter_FOUND AND SEED_OPENSSL)
        set(seed_update_test "${CMAKE_CURRENT_BINARY_DIR}/update-test")
        set(seed_update_app "${seed_update_test}/Applications/Seed Editor.app")
        string(REPLACE "%" "%25" seed_update_url "${seed_update_test}/feed/update.json")
        string(REPLACE " " "%20" seed_update_url "file://${seed_update_url}")
        add_test(NAME editor_update_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${seed_update_test}")
        add_test(NAME editor_update_release COMMAND ${seed_editor_bundle_command} -DVERSION=99.0.0
            "-DBUNDLE=${seed_update_test}/release/Seed Editor.app" -P "${seed_editor_bundle_script}")
        add_test(NAME editor_update_feed COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/release_editor.py"
            package "${seed_update_test}/release/Seed Editor.app" "${seed_update_test}/feed"
            --key "${CMAKE_CURRENT_SOURCE_DIR}/tests/update/test-key.pem")
        add_test(NAME editor_update_installed COMMAND /usr/bin/ditto "${seed_editor_bundle}" "${seed_update_app}")
        add_test(NAME editor_update COMMAND "${seed_update_app}/Contents/MacOS/seed_editor" --smoke
            --preferences "${seed_update_test}/preferences" --update-feed "${seed_update_url}" --update-key
            04d1902a9ce5970d41cca0f0095758f327b47546ef3a5679efa846fe43776046d254af9a03a10ec0e16a920eb0333e1c3bfc984606b9710eb2b92d8e3cd3251142)
        add_test(NAME editor_update_installed_version COMMAND /usr/bin/plutil -extract CFBundleShortVersionString raw
            "${seed_update_app}/Contents/Info.plist")
        add_test(NAME editor_update_runs COMMAND "${seed_update_app}/Contents/MacOS/seed_editor" --version)
        set_tests_properties(editor_update_release editor_update_installed PROPERTIES DEPENDS editor_update_clean)
        set_tests_properties(editor_update_feed PROPERTIES DEPENDS editor_update_release)
        set_tests_properties(editor_update PROPERTIES DEPENDS "editor_update_feed;editor_update_installed" TIMEOUT 120
            PASS_REGULAR_EXPRESSION "downloaded and verified.*Installed Seed Editor 99\\.0\\.0\\."
            FAIL_REGULAR_EXPRESSION "Could not")
        set_tests_properties(editor_update_installed_version PROPERTIES DEPENDS editor_update
            PASS_REGULAR_EXPRESSION "^99\\.0\\.0")
        set_tests_properties(editor_update_runs PROPERTIES DEPENDS editor_update PASS_REGULAR_EXPRESSION "^Seed Editor ")
    else()
        message(STATUS "Skipping the Seed Editor.app update test: it needs macOS, Python 3 and openssl")
    endif()
    # Scripted mouse and keyboard editing of a fresh copy of the sample project.
    add_test(NAME editor_ui_copy COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/tests/sample_project" "${seed_editor_smoke}/UiTest")
    add_test(NAME editor_ui COMMAND seed_editor --smoke --ui-test --preferences "${seed_editor_smoke}/preferences"
        --open "${seed_editor_smoke}/UiTest")
    set_tests_properties(editor_ui_copy PROPERTIES DEPENDS editor_smoke_clean)
    set_tests_properties(editor_ui PROPERTIES DEPENDS editor_ui_copy TIMEOUT 120)
endif()
