cmake_minimum_required(VERSION 3.21)
project(harmony_extension LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GAMESCRCPY_SOURCE "" CACHE PATH "GameScrcpy source tree containing third_party/ffmpeg")
add_library(native SHARED src/harmony.cpp)
target_include_directories(native PRIVATE include "${GAMESCRCPY_SOURCE}/third_party/ffmpeg/include")
target_link_libraries(native PRIVATE ws2_32
    "${GAMESCRCPY_SOURCE}/third_party/ffmpeg/lib/avcodec.lib"
    "${GAMESCRCPY_SOURCE}/third_party/ffmpeg/lib/avutil.lib")
set_target_properties(native PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}$<0:>"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/symbols$<0:>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
if(MSVC)
    target_compile_options(native PRIVATE /W4 /permissive- /utf-8)
endif()
