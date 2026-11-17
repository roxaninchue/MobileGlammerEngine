cmake_minimum_required(VERSION 3.22)
project(mge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MGE_PLATFORM_ANDROID "Build the Android platform layer" OFF)

add_library(mge_core STATIC
    src/core/log.cpp
)
target_include_directories(mge_core PUBLIC include)
target_compile_options(mge_core PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti
    $<$<CONFIG:Release>:-O2 -flto>)

if(MGE_PLATFORM_ANDROID)
    add_library(mge SHARED
        src/platform/android/jni_bridge.cpp
        src/platform/android/engine_host.cpp
    )
    target_link_libraries(mge PRIVATE mge_core android log)
    target_compile_options(mge PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
    target_link_options(mge PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
endif()
