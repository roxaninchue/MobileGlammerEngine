cmake_minimum_required(VERSION 3.22)
project(mge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MGE_PLATFORM_ANDROID "Build the Android platform layer" OFF)

add_library(mge_core STATIC
    src/core/log.cpp
    src/core/math.cpp
    src/core/memory.cpp
    src/core/jobs.cpp
    src/core/ecs.cpp
    src/scene/transform2d.cpp
    src/scene/serialize.cpp
    src/assets/texture.cpp
    src/render/sprite_batch.cpp
    src/render/camera.cpp
    src/render/tilemap.cpp
    src/input/input.cpp
    src/physics2d/shapes.cpp
    src/physics2d/broadphase.cpp
    src/physics2d/manifold.cpp
    src/physics2d/world.cpp
    src/physics2d/physics_system.cpp
    src/render/sprite_system.cpp
    src/perf/frame_stats.cpp
    src/demo/demo_scene.cpp
    src/editor/scene_doc.cpp
    src/editor/command.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(mge_core PUBLIC Threads::Threads)
target_include_directories(mge_core PUBLIC include)
target_compile_options(mge_core PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti
    "$<$<CONFIG:Release>:-O2;-flto>")

if(MGE_PLATFORM_ANDROID)
    add_library(mge SHARED
        src/platform/android/jni_bridge.cpp
        src/platform/android/engine_host.cpp
        src/render/renderer.cpp
        src/render/vulkan/vk_renderer.cpp
        src/render/gles/gles_renderer.cpp
    )
    target_link_libraries(mge PRIVATE mge_core android log vulkan EGL GLESv3)
    target_compile_options(mge PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
    target_link_options(mge PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
endif()

# Host unit tests (CI runs these on Linux; Android builds skip them).
option(MGE_BUILD_TESTS "Build host unit tests" OFF)
if(MGE_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(mge_tests tests/test_main.cpp tests/test_math.cpp tests/test_memory.cpp tests/test_jobs.cpp tests/test_ecs.cpp tests/test_transform2d.cpp tests/test_serialize.cpp tests/test_texture.cpp tests/test_sprite_batch.cpp tests/test_camera.cpp tests/test_tilemap.cpp tests/test_input.cpp tests/test_physics2d.cpp tests/test_physics2d_world.cpp tests/test_demo.cpp
        tests/test_editor_undo.cpp)
    target_link_libraries(mge_tests PRIVATE mge_core)
    target_compile_definitions(mge_tests PRIVATE MGE_FORCE_ASSERTS)
    add_test(NAME mge_tests COMMAND mge_tests)
    # Headless benchmark of the M1 demo scene (prints a report; not a pass/fail test).
    add_executable(mge_bench bench/bench_main.cpp)
    target_link_libraries(mge_bench PRIVATE mge_core)
endif()
