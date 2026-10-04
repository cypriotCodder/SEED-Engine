cmake_minimum_required(VERSION 3.24)
project(SeedEngine VERSION 0.1.0 LANGUAGES C CXX)
include(FetchContent)
FetchContent_Declare(lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
    URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(lz4)
add_library(seed_lz4 STATIC ${lz4_SOURCE_DIR}/lib/lz4.c)
target_include_directories(seed_lz4 SYSTEM PUBLIC ${lz4_SOURCE_DIR}/lib)
find_package(Threads REQUIRED)

option(SEED_FETCH_SDL "Download the pinned SDL2 dependency" ON)
option(SEED_BUILD_DEMO "Build the demo game in games/demo" ON)
option(SEED_BUILD_EDITOR "Build the editor application in editor/" ON)
option(SEED_WARNINGS_AS_ERRORS "Treat engine warnings as errors" ON)

if(SEED_FETCH_SDL)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        URL https://www.libsdl.org/release/SDL2-2.30.11.tar.gz
        URL_HASH SHA256=8b8d4aef2038533da814965220f88f77d60dfa0f32685f80ead65e501337da7f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(SDL2)
else()
    find_package(SDL2 2.0.18 CONFIG REQUIRED)
endif()

# Dear ImGui (docking release) draws the editor app. Games never link it. Built as its own library
# without the engine's strict warnings.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
    URL_HASH SHA256=90ded916bd57db2e0e171b6b098940a47c6f5042725dcdc67fb19940ca8bfdcc
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)
add_library(seed_imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(seed_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_features(seed_imgui PUBLIC cxx_std_20)
target_link_libraries(seed_imgui PUBLIC SDL2::SDL2)

# Lua 5.4 runs projects' gameplay scripts. Only code that uses the script host links it in.
FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.4.9.tar.gz
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(lua)
file(GLOB seed_lua_sources ${lua_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM seed_lua_sources ${lua_SOURCE_DIR}/src/lua.c ${lua_SOURCE_DIR}/src/luac.c)
# Compiled as C++ so Lua errors are C++ exceptions and unwind engine frames properly.
set_source_files_properties(${seed_lua_sources} PROPERTIES LANGUAGE CXX)
add_library(seed_lua STATIC ${seed_lua_sources})
target_include_directories(seed_lua SYSTEM PUBLIC ${lua_SOURCE_DIR}/src)
if(NOT WIN32)
    target_compile_definitions(seed_lua PRIVATE LUA_USE_POSIX)
endif()

# stb_image decodes PNG, JPG and TGA files the editor imports as textures. Editor only: games get
# textures already compressed in their asset pack.
FetchContent_Declare(stb
    URL https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h
    URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(stb)

function(seed_strict target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /fp:strict)
        if(SEED_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
        if(SEED_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
add_library(seed_engine STATIC src/platform/window.cpp src/render/renderer.cpp src/world/world.cpp src/io/storage.cpp src/physics/physics.cpp
    src/world/chunk_file.cpp src/io/checkpoint.cpp src/io/json.cpp src/project/assets.cpp src/project/scene_file.cpp src/project/terrain.cpp src/project/paint.cpp src/project/atmosphere.cpp src/project/runner.cpp src/project/archive.cpp src/project/characters.cpp src/project/game_settings.cpp src/script/host.cpp src/core/metrics.cpp
    src/app/app.cpp)
seed_strict(seed_engine)
target_include_directories(seed_engine PUBLIC src)
target_compile_definitions(seed_engine PUBLIC SDL_MAIN_HANDLED)
target_link_libraries(seed_engine PUBLIC SDL2::SDL2 seed_lz4 Threads::Threads PRIVATE seed_lua)
if(WIN32)
    target_link_libraries(seed_engine PUBLIC psapi)
endif()
# The runtime that plays a project; see player/main.cpp.
add_executable(seed_player player/main.cpp)
seed_strict(seed_player)
target_link_libraries(seed_player PRIVATE seed_engine)
install(TARGETS seed_player RUNTIME DESTINATION bin)

add_executable(seed_checkpoint_bench tools/checkpoint_benchmark.cpp)
seed_strict(seed_checkpoint_bench)
target_link_libraries(seed_checkpoint_bench PRIVATE seed_engine)
install(FILES licenses/SDL2.txt licenses/LZ4.txt licenses/ImGui.txt licenses/Lua.txt DESTINATION share/licenses/seed_engine)

add_executable(seed_cook tools/cook.cpp src/io/storage.cpp)
target_include_directories(seed_cook PRIVATE src)
seed_strict(seed_cook)
target_link_libraries(seed_cook PRIVATE seed_lz4)
if(CMAKE_CROSSCOMPILING)
    set(SEED_HOST_COOKER "" CACHE FILEPATH "Native seed_cook executable for cross builds")
    if(NOT EXISTS "${SEED_HOST_COOKER}")
        message(FATAL_ERROR "Cross builds require SEED_HOST_COOKER from a native build")
    endif()
    set(seed_cooker "${SEED_HOST_COOKER}")
else()
    set(seed_cooker seed_cook)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/demo.pak"
    COMMAND ${seed_cooker} "${CMAKE_CURRENT_BINARY_DIR}/demo.pak" --builtin
    DEPENDS ${seed_cooker} VERBATIM)
add_custom_target(seed_assets ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/demo.pak")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/demo.pak" DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(seed_core_tests tests/core_tests.cpp)
    target_include_directories(seed_core_tests PRIVATE src)
    seed_strict(seed_core_tests)
    add_test(NAME core_logic COMMAND seed_core_tests)
    add_executable(seed_storage_tests tests/storage_tests.cpp src/io/storage.cpp)
    seed_strict(seed_storage_tests)
    target_include_directories(seed_storage_tests PRIVATE src)
    target_link_libraries(seed_storage_tests PRIVATE seed_lz4 Threads::Threads)
    add_dependencies(seed_storage_tests seed_assets)
    add_executable(seed_checkpoint_tests tests/checkpoint_tests.cpp src/io/checkpoint.cpp src/io/storage.cpp)
    seed_strict(seed_checkpoint_tests)
    target_include_directories(seed_checkpoint_tests PRIVATE src)
    target_link_libraries(seed_checkpoint_tests PRIVATE seed_lz4 Threads::Threads)
    add_test(NAME checkpoint_recovery COMMAND seed_checkpoint_tests "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-test-data")
    add_executable(seed_physics_tests tests/physics_tests.cpp)
    seed_strict(seed_physics_tests)
    target_link_libraries(seed_physics_tests PRIVATE seed_engine)
    add_test(NAME physics_logic COMMAND seed_physics_tests)
    add_executable(seed_json_tests tests/json_tests.cpp src/io/json.cpp)
    target_include_directories(seed_json_tests PRIVATE src)
    seed_strict(seed_json_tests)
    add_test(NAME json COMMAND seed_json_tests)
    add_executable(seed_assets_tests tests/assets_tests.cpp)
    seed_strict(seed_assets_tests)
    target_link_libraries(seed_assets_tests PRIVATE seed_engine)
    add_test(NAME project_assets COMMAND seed_assets_tests "${CMAKE_CURRENT_BINARY_DIR}/assets-test-data")
    add_executable(seed_scene_tests tests/scene_tests.cpp)
    seed_strict(seed_scene_tests)
    target_link_libraries(seed_scene_tests PRIVATE seed_engine)
    add_test(NAME scene_files COMMAND seed_scene_tests "${CMAKE_CURRENT_BINARY_DIR}/scene-test-data")
    add_executable(seed_terrain_tests tests/terrain_tests.cpp)
    seed_strict(seed_terrain_tests)
    target_link_libraries(seed_terrain_tests PRIVATE seed_engine)
    add_test(NAME terrain COMMAND seed_terrain_tests)
    add_executable(seed_archive_tests tests/archive_tests.cpp)
    seed_strict(seed_archive_tests)
    target_link_libraries(seed_archive_tests PRIVATE seed_engine)
    add_test(NAME project_archive COMMAND seed_archive_tests "${CMAKE_CURRENT_BINARY_DIR}/archive-test-data")
    add_executable(seed_input_tests tests/input_tests.cpp)
    seed_strict(seed_input_tests)
    target_link_libraries(seed_input_tests PRIVATE seed_engine)
    add_test(NAME input_actions COMMAND seed_input_tests)
    add_executable(seed_character_tests tests/character_tests.cpp)
    seed_strict(seed_character_tests)
    target_link_libraries(seed_character_tests PRIVATE seed_engine)
    add_test(NAME character_movement COMMAND seed_character_tests)
    add_executable(seed_effects_tests tests/effects_tests.cpp)
    seed_strict(seed_effects_tests)
    target_link_libraries(seed_effects_tests PRIVATE seed_engine)
    add_test(NAME effects COMMAND seed_effects_tests)
    # A second, minimal game proves the engine works without the demo. It opens a real window.
    add_executable(seed_minimal_game tests/minimal_game.cpp)
    seed_strict(seed_minimal_game)
    target_link_libraries(seed_minimal_game PRIVATE seed_engine)
    add_custom_command(TARGET seed_minimal_game POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/minimal_game_assets" "$<TARGET_FILE_DIR:seed_minimal_game>/minimal_game_assets")
    # A project with no game code, run by seed_player: once on a fresh save, then resumed.
    set(seed_sample_save "${CMAKE_CURRENT_BINARY_DIR}/sample-project-save")
    add_test(NAME player_fresh_save COMMAND ${CMAKE_COMMAND} -E rm -rf "${seed_sample_save}")
    add_test(NAME player COMMAND seed_player --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/sample_project" --smoke
        --save "${seed_sample_save}")
    add_test(NAME player_resume COMMAND seed_player --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/sample_project"
        --smoke --save "${seed_sample_save}")
    set_tests_properties(player PROPERTIES DEPENDS player_fresh_save)
    set_tests_properties(player_resume PROPERTIES DEPENDS player)
    # Gameplay scripts: an API walkthrough that must pass, and broken scripts that must be caught
    # (a syntax error and an endless loop) without hanging the game.
    add_test(NAME scripts_fresh_save COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/script-project-save"
        "${CMAKE_CURRENT_BINARY_DIR}/surfaces-save" "${CMAKE_CURRENT_BINARY_DIR}/atmosphere-save"
        "${CMAKE_CURRENT_BINARY_DIR}/gameplay-save")
    add_test(NAME scripts COMMAND seed_player --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/script_project" --smoke
        --save "${CMAKE_CURRENT_BINARY_DIR}/script-project-save")
    set_tests_properties(scripts PROPERTIES PASS_REGULAR_EXPRESSION "Script checks passed.*Character checks passed|Character checks passed.*Script checks passed"
        FAIL_REGULAR_EXPRESSION "Script error|Engine error")
    # The same project's painted terrain and ground materials, from a walking NPC's script.
    add_test(NAME surfaces COMMAND seed_player --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/script_project" --smoke
        --save "${CMAKE_CURRENT_BINARY_DIR}/surfaces-save")
    set_tests_properties(surfaces PROPERTIES PASS_REGULAR_EXPRESSION "Surface checks passed"
        FAIL_REGULAR_EXPRESSION "Script error|Engine error" DEPENDS scripts_fresh_save)
    set_tests_properties(scripts PROPERTIES DEPENDS scripts_fresh_save)
    # Touch events, nearby queries, ui, the pointer and pausing, in a project of their own because
    # pausing holds every other script's world still.
    add_test(NAME gameplay COMMAND seed_player --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/gameplay_project" --smoke
        --save "${CMAKE_CURRENT_BINARY_DIR}/gameplay-save")
    set_tests_properties(gameplay PROPERTIES PASS_REGULAR_EXPRESSION "Gameplay checks passed"
        FAIL_REGULAR_EXPRESSION "Script error|Engine error" DEPENDS scripts_fresh_save)
    add_test(NAME atmosphere COMMAND seed_player --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/script_project" --smoke
        --save "${CMAKE_CURRENT_BINARY_DIR}/atmosphere-save")
    set_tests_properties(atmosphere PROPERTIES PASS_REGULAR_EXPRESSION "Atmosphere checks passed"
        FAIL_REGULAR_EXPRESSION "Script error|Engine error" DEPENDS scripts_fresh_save)
    add_test(NAME start_scene COMMAND seed_player --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/start_scene_project"
        --smoke --save "${CMAKE_CURRENT_BINARY_DIR}/start-scene-save")
    set_tests_properties(start_scene PROPERTIES PASS_REGULAR_EXPRESSION "Started in level2"
        FAIL_REGULAR_EXPRESSION "Script error|Engine error")
    add_test(NAME script_errors COMMAND seed_player --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/script_errors" --smoke
        --save "${CMAKE_CURRENT_BINARY_DIR}/script-errors-save")
    set_tests_properties(script_errors PROPERTIES TIMEOUT 30
        PASS_REGULAR_EXPRESSION "syntax.lua:3:.*endless loop.*2 script error")
    add_test(NAME minimal_game_fresh_save COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/minimal-game-save")
    add_test(NAME minimal_game COMMAND seed_minimal_game --smoke --save "${CMAKE_CURRENT_BINARY_DIR}/minimal-game-save")
    set_tests_properties(minimal_game PROPERTIES DEPENDS minimal_game_fresh_save)
    add_test(NAME minimal_game_reload COMMAND seed_minimal_game --smoke --save "${CMAKE_CURRENT_BINARY_DIR}/minimal-game-save")
    set_tests_properties(minimal_game_reload PROPERTIES DEPENDS minimal_game)
    add_test(NAME storage_logic COMMAND seed_storage_tests "${CMAKE_CURRENT_BINARY_DIR}/storage-test-data" "${CMAKE_CURRENT_BINARY_DIR}/demo.pak")
endif()

if(SEED_BUILD_DEMO)
    add_subdirectory(games/demo)
endif()

if(SEED_BUILD_EDITOR)
    add_subdirectory(editor)
endif()
