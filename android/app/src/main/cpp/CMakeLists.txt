cmake_minimum_required(VERSION 3.22)
project(native_spotify_android LANGUAGES C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(repo ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

if(NOT SPOTIFY_CLIENT_ID MATCHES "^[0-9a-fA-F]+$")
  message(FATAL_ERROR "Pass -DSPOTIFY_CLIENT_ID from Gradle; it comes from the root CMakeLists.txt.")
endif()

# The same shared core the desktop builds, plus the Android platform adapter.
add_library(native_spotify_android SHARED
    bridge.c
    ${repo}/src/core/core.c
    ${repo}/src/core/catalog.c
    ${repo}/src/core/collection.c
    ${repo}/src/core/browse.c
    ${repo}/src/core/browse_cache.c
    ${repo}/src/core/player.c
    ${repo}/src/core/lyrics.c
    ${repo}/src/core/api.c
    ${repo}/src/core/artwork.c
    ${repo}/src/core/engine.c
    ${repo}/src/core/soloist.c
    ${repo}/src/core/app.c
    ${repo}/src/core/app_content.c
    ${repo}/src/core/app_queue.c
    ${repo}/src/core/app_workers.c
    ${repo}/src/platform/loopback.c
    ${repo}/src/platform/posix_common.c
    ${repo}/src/platform/android_system.c
    ${repo}/src/ui/icons.c
    ${repo}/src/ui/canvas_raster.c
    ${repo}/vendor/yyjson/yyjson.c)
target_include_directories(native_spotify_android PRIVATE
    ${repo}/src ${repo}/src/core ${repo}/src/ui ${repo}/src/platform ${repo}/vendor/yyjson)
target_compile_definitions(native_spotify_android PRIVATE
    NATIVE_SPOTIFY_CLIENT_ID="${SPOTIFY_CLIENT_ID}" YYJSON_DISABLE_UTILS=1)
target_compile_options(native_spotify_android PRIVATE -ffunction-sections -fdata-sections -Wall -Wextra)
target_link_options(native_spotify_android PRIVATE -Wl,--gc-sections)
set_target_properties(native_spotify_android PROPERTIES C_VISIBILITY_PRESET hidden)
target_link_libraries(native_spotify_android PRIVATE m)
