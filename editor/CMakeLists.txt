# The Seed editor: a separate application for creating and building game projects. Games never link
# it or Dear ImGui.
add_library(seed_editor_core STATIC project.cpp starter.cpp scripts.cpp)
seed_strict(seed_editor_core)
target_include_directories(seed_editor_core PUBLIC .)
target_link_libraries(seed_editor_core PUBLIC seed_engine PRIVATE seed_lua)

if(APPLE)
    set(seed_folder_dialog folder_dialog_mac.mm)
else()
    set(seed_folder_dialog folder_dialog_none.cpp)
endif()
add_executable(seed_editor main.cpp app.cpp asset_panels.cpp terrain_panel.cpp scene_editor.cpp play.cpp ${seed_folder_dialog})
seed_strict(seed_editor)
target_link_libraries(seed_editor PRIVATE seed_editor_core seed_imgui)
add_dependencies(seed_editor seed_player)
target_compile_definitions(seed_editor PRIVATE SEED_PLAYER_PATH="$<TARGET_FILE:seed_player>")
if(APPLE)
    target_link_libraries(seed_editor PRIVATE "-framework AppKit")
    # Compiled as C++ with an Objective-C++ flag, so the project needs no OBJCXX language.
    set_source_files_properties(folder_dialog_mac.mm PROPERTIES LANGUAGE CXX
        COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
endif()
install(TARGETS seed_editor RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    add_executable(seed_editor_project_tests tests/project_tests.cpp)
    seed_strict(seed_editor_project_tests)
    target_link_libraries(seed_editor_project_tests PRIVATE seed_editor_core)
    add_executable(seed_editor_history_tests tests/history_tests.cpp)
    seed_strict(seed_editor_history_tests)
    target_include_directories(seed_editor_history_tests PRIVATE .)
    add_test(NAME editor_history COMMAND seed_editor_history_tests)
    add_test(NAME editor_projects COMMAND seed_editor_project_tests "${CMAKE_CURRENT_BINARY_DIR}/project-test-data")
    # Creates a project through the editor, then reopens it, drawing 60 frames each time.
    set(seed_editor_smoke "${CMAKE_CURRENT_BINARY_DIR}/editor-smoke")
    add_test(NAME editor_smoke_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${seed_editor_smoke}")
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
endif()
