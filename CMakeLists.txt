cmake_minimum_required(VERSION 3.16)
project(happlayer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)  # 源文件含中文，按 UTF-8 编译
endif()

# --- snappy（源码构建） ---
set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SNAPPY_INSTALL OFF CACHE BOOL "" FORCE)
set(SNAPPY_FUZZING_BUILD OFF CACHE BOOL "" FORCE)
set(SNAPPY_REQUIRE_AVX OFF CACHE BOOL "" FORCE)
set(SNAPPY_REQUIRE_AVX2 OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/snappy EXCLUDE_FROM_ALL)

# --- FFmpeg（gyan full_build-shared 预编译包，解压到 deps/ffmpeg） ---
set(FFMPEG_DIR ${CMAKE_SOURCE_DIR}/deps/ffmpeg)

# --- GLFW 预编译库按编译器选目录 ---
if(MSVC)
    set(GLFW_LIB_DIR ${CMAKE_SOURCE_DIR}/deps/glfw/lib-vc2022)
else()
    set(GLFW_LIB_DIR ${CMAKE_SOURCE_DIR}/deps/glfw/lib-mingw-w64)
endif()

# --- SDL2（预编译开发包，解压到 deps/sdl2，含 cmake/sdl2-config.cmake） ---
set(SDL2_DIR ${CMAKE_SOURCE_DIR}/deps/sdl2/cmake)
# 找包（用我们自带的 cmake config；如未提供则回退到 find_path）
find_package(SDL2 CONFIG QUIET)
if(MSVC)
    set(_SDL2_LIB_X64_DIR ${CMAKE_SOURCE_DIR}/deps/sdl2/lib/x64)
else()
    set(_SDL2_LIB_X64_DIR ${CMAKE_SOURCE_DIR}/deps/sdl2/lib/x86)
endif()
if(NOT SDL2_FOUND)
    # 备用：手动指定 include/lib，让 find_package 行为仍可预期
    add_library(SDL2::SDL2 SHARED IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${_SDL2_LIB_X64_DIR}/SDL2.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/deps/sdl2/include"
        INTERFACE_LINK_LIBRARIES "${_SDL2_LIB_X64_DIR}/SDL2main.lib")
endif()

# --- Vulkan（deps/vulkan 自带 header + lib） ---
# 检测 deps/vulkan/include/vulkan/vulkan.h + deps/vulkan/lib/vulkan-1.lib
# 只要二者都存在即启用 HAPPLAYER_HAS_VULKAN=1（编译 vulkan_hapq.cpp，main.cpp #ifdef 调用）
set(VULKAN_DIR ${CMAKE_SOURCE_DIR}/deps/vulkan)
if(EXISTS ${VULKAN_DIR}/include/vulkan/vulkan.h AND EXISTS ${VULKAN_DIR}/lib/vulkan-1.lib)
    set(HAPPLAYER_HAS_VULKAN 1)
    message(STATUS "[happlayer] 检测到 Vulkan SDK → 启用 HAPPLAYER_HAS_VULKAN=1")
else()
    message(STATUS "[happlayer] 未检测到 Vulkan SDK → 跳过 vulkan_hapq.cpp 编译")
endif()

add_executable(happlayer
    src/main.cpp
    src/audio.cpp
    $<$<BOOL:${HAPPLAYER_HAS_VULKAN}>:src/vulkan_hapq.cpp>
    $<$<BOOL:${HAPPLAYER_HAS_VULKAN}>:src/vulkan_renderer.cpp>)
target_include_directories(happlayer PRIVATE
    ${FFMPEG_DIR}/include
    ${CMAKE_SOURCE_DIR}/deps/glfw/include
    ${CMAKE_SOURCE_DIR}/deps/sdl2/include
    $<$<BOOL:${HAPPLAYER_HAS_VULKAN}>:${VULKAN_DIR}/include>)
target_link_directories(happlayer PRIVATE
    ${FFMPEG_DIR}/lib
    ${GLFW_LIB_DIR}
    ${_SDL2_LIB_X64_DIR}
    $<$<BOOL:${HAPPLAYER_HAS_VULKAN}>:${VULKAN_DIR}/lib>)
target_link_libraries(happlayer PRIVATE
    avformat avcodec avutil swresample swscale
    glfw3 opengl32 gdi32 user32 shell32
    ws2_32 dwmapi winmm
    SDL2::SDL2 SDL2::SDL2main
    snappy
    $<$<BOOL:${HAPPLAYER_HAS_VULKAN}>:vulkan-1>)
if(HAPPLAYER_HAS_VULKAN)
    target_compile_definitions(happlayer PRIVATE HAPPLAYER_HAS_VULKAN=1)
endif()
if(MINGW)
    target_link_options(happlayer PRIVATE -static-libgcc -static-libstdc++)
endif()

# 运行时需要的 FFmpeg DLL + SDL2 DLL 拷到 exe 旁边
add_custom_command(TARGET happlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${FFMPEG_DIR}/bin $<TARGET_FILE_DIR:happlayer>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/deps/sdl2/lib/x64/SDL2.dll $<TARGET_FILE_DIR:happlayer>/SDL2.dll)

# 配置文件放到 exe 旁边，方便直接改；测试素材存在则一并拷过去
add_custom_command(TARGET happlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/layers.txt $<TARGET_FILE_DIR:happlayer>/layers.txt)
if(EXISTS ${CMAKE_SOURCE_DIR}/clips)
    add_custom_command(TARGET happlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/clips $<TARGET_FILE_DIR:happlayer>/clips)
endif()

# --- 解码器单元测试 ---
enable_testing()
add_executable(test_hapdecode tests/test_hapdecode.cpp)
target_link_libraries(test_hapdecode PRIVATE snappy)
add_test(NAME hapdecode COMMAND test_hapdecode)

# --- 临时诊断工具：dump HAP Q 帧的 DXT5 alpha 端点 ---
add_executable(dump_hap tests/dump_hap.cpp)
target_include_directories(dump_hap PRIVATE ${FFMPEG_DIR}/include)
target_link_directories(dump_hap PRIVATE ${FFMPEG_DIR}/lib)
target_link_libraries(dump_hap PRIVATE avformat avcodec avutil snappy)
