# The demo game: a finite procedural island world with a destructible timber platform.
# It links only seed_engine; engine code never includes anything from games/.
add_executable(seed_demo main.cpp)
seed_strict(seed_demo)
target_link_libraries(seed_demo PRIVATE seed_engine)
if(MINGW)
    target_link_options(seed_demo PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_dependencies(seed_demo seed_assets)
add_custom_command(TARGET seed_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/demo.pak" "$<TARGET_FILE_DIR:seed_demo>/demo.pak")
install(TARGETS seed_demo RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    add_executable(seed_demo_world_tests tests/world_tests.cpp)
    target_include_directories(seed_demo_world_tests PRIVATE .)
    seed_strict(seed_demo_world_tests)
    target_link_libraries(seed_demo_world_tests PRIVATE seed_engine)
    add_test(NAME demo_world_generation COMMAND seed_demo_world_tests)
endif()
