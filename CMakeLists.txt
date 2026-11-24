cmake_minimum_required(VERSION 3.20)
project(native_spotify VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(ENABLE_SANITIZERS "Enable address/undefined sanitizers" OFF)
set(SPOTIFY_CLIENT_ID "0147b268b69346959cdf54f65609db9e" CACHE STRING "Public OAuth client ID for this application")
string(LENGTH "${SPOTIFY_CLIENT_ID}" CLIENT_ID_LENGTH)

if(NOT CLIENT_ID_LENGTH EQUAL 32 OR NOT SPOTIFY_CLIENT_ID MATCHES "^[0-9a-fA-F]+$")
  message(FATAL_ERROR "SPOTIFY_CLIENT_ID must be the application's 32-character public client ID")
endif()

add_library(yyjson STATIC vendor/yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC vendor/yyjson)
target_compile_definitions(yyjson PRIVATE YYJSON_DISABLE_UTILS=1)

add_library(core STATIC
  src/core/core.c
  src/core/catalog.c
  src/core/collection.c
  src/core/browse.c
  src/core/browse_cache.c
  src/core/player.c
  src/core/lyrics.c
  src/core/api.c
  src/core/artwork.c
  src/core/engine.c
  src/core/soloist.c
  src/core/app.c
  src/core/app_content.c
  src/core/app_queue.c
  src/core/app_workers.c
  src/platform/loopback.c
)
target_include_directories(core PUBLIC src src/core src/platform)
target_compile_definitions(core PUBLIC NATIVE_SPOTIFY_CLIENT_ID="${SPOTIFY_CLIENT_ID}")
target_link_libraries(core PUBLIC yyjson)

add_library(presentation STATIC
  src/ui/controller.c
  src/ui/scene.c
  src/ui/scene_actions.c
  src/ui/scene_input.c
  src/ui/scene_draw.c
  src/ui/search_scene.c
  src/ui/home_scene.c
  src/ui/panel_scene.c
  src/ui/lyrics_scene.c
  src/ui/collection_scene.c
  src/ui/settings_scene.c
  src/ui/scroll.c
  src/ui/icons.c
  src/ui/canvas_raster.c
  src/ui/artwork_cache.c
  src/ui/artwork_store.c
)
target_include_directories(presentation PUBLIC src/ui)
target_link_libraries(presentation PUBLIC core)

if(WIN32)
  target_sources(core PRIVATE src/platform/win32_system.c src/platform/win32_image.c src/platform/win32_engine.c)
  target_compile_definitions(core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0a00)
  target_link_libraries(core PUBLIC winhttp bcrypt advapi32 shell32 ws2_32 ole32 windowscodecs uuid)
  set(NATIVE_UI src/platform/win32_window.c src/platform/win32_canvas.c)
  set(NATIVE_RC "${CMAKE_CURRENT_BINARY_DIR}/native_spotify.rc")
  file(WRITE "${NATIVE_RC}" "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/assets/native_spotify.ico\"\n")

  if(ZIG_EXECUTABLE)
    find_program(LLVM_WINDRES NAMES llvm-windres x86_64-w64-mingw32-windres windres)
    if(LLVM_WINDRES)
      set(NATIVE_RES "${CMAKE_CURRENT_BINARY_DIR}/native_spotify_res.o")
      add_custom_command(
        OUTPUT "${NATIVE_RES}"
        COMMAND "${LLVM_WINDRES}" --target=pe-x86-64 "${NATIVE_RC}" -O coff -o "${NATIVE_RES}"
        DEPENDS "${NATIVE_RC}" "${CMAKE_CURRENT_SOURCE_DIR}/assets/native_spotify.ico"
        VERBATIM
      )
      list(APPEND NATIVE_UI "${NATIVE_RES}")
    endif()
  else()
    list(APPEND NATIVE_UI "${NATIVE_RC}")
  endif()

  add_executable(native_spotify WIN32 src/main.c ${NATIVE_UI})
  target_link_libraries(native_spotify PRIVATE presentation gdi32 user32 msimg32 dwmapi)

  if(ZIG_EXECUTABLE)
    target_link_options(native_spotify PRIVATE -Wl,--subsystem,windows)
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)

  pkg_check_modules(PLATFORM REQUIRED IMPORTED_TARGET libcurl openssl libsecret-1)
  pkg_check_modules(UI REQUIRED IMPORTED_TARGET x11 cairo pangocairo fontconfig)
  pkg_check_modules(WEBP QUIET IMPORTED_TARGET libwebp)
  find_package(JPEG REQUIRED)
  find_package(PNG REQUIRED)

  find_package(Threads REQUIRED)

  target_sources(core PRIVATE src/platform/posix_common.c src/platform/linux_system.c src/platform/linux_image.c src/platform/linux_engine.c)
  target_link_libraries(core PUBLIC PkgConfig::PLATFORM Threads::Threads JPEG::JPEG PNG::PNG)
  target_link_libraries(presentation PUBLIC m)

  if(WEBP_FOUND)
    target_compile_definitions(core PRIVATE HAVE_LIBWEBP=1)
    target_link_libraries(core PUBLIC PkgConfig::WEBP)
  endif()

  set(NATIVE_UI src/platform/linux_window.c src/platform/linux_canvas.c)
  add_executable(native_spotify src/main.c ${NATIVE_UI})
  target_link_libraries(native_spotify PRIVATE presentation PkgConfig::UI)
else()
  message(FATAL_ERROR "Frontends implemented: Windows and Linux. macOS adapter is not implemented yet.")
endif()

# Soloist is downloaded from Spotify at runtime; Spotify does not allow redistributing it.
install(TARGETS native_spotify RUNTIME DESTINATION .)
install(DIRECTORY assets DESTINATION .)
install(FILES README.md DESTINATION .)
set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_FILE_NAME "native-spotify-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
include(CPack)

add_custom_command(TARGET native_spotify POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:native_spotify>/assets)

if(MSVC)
  target_compile_options(core PUBLIC /utf-8)
  target_compile_options(core PRIVATE /W4)
  target_compile_options(native_spotify PRIVATE /W4)
  target_compile_options(presentation PRIVATE /W4)
else()
  foreach(target core presentation native_spotify)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endforeach()

  if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)

    foreach(target core yyjson presentation native_spotify)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endforeach()

    target_link_options(native_spotify PRIVATE -fsanitize=address,undefined)
  endif()
endif()

include(CTest)

if(BUILD_TESTING)
  add_executable(core_tests tests/core_tests.c)
  target_link_libraries(core_tests PRIVATE core)
  add_test(NAME core_tests COMMAND core_tests)
  set_tests_properties(core_tests PROPERTIES TIMEOUT 15)

  add_executable(browse_tests tests/browse_tests.c)
  target_link_libraries(browse_tests PRIVATE core)
  add_test(NAME browse_tests COMMAND browse_tests)

  add_executable(player_tests tests/player_tests.c)
  target_link_libraries(player_tests PRIVATE core)
  add_test(NAME player_tests COMMAND player_tests)

  add_executable(artwork_tests tests/artwork_tests.c)
  target_link_libraries(artwork_tests PRIVATE core)
  add_test(NAME artwork_tests COMMAND artwork_tests)

  add_executable(artwork_store_tests tests/artwork_store_tests.c)
  target_link_libraries(artwork_store_tests PRIVATE presentation)
  add_test(NAME artwork_store_tests COMMAND artwork_store_tests)

  add_executable(engine_tests tests/engine_tests.c)
  target_link_libraries(engine_tests PRIVATE core)
  add_test(NAME engine_tests COMMAND engine_tests)
  set_tests_properties(engine_tests PROPERTIES TIMEOUT 30)

  add_executable(interaction_tests tests/interaction_tests.c ${NATIVE_UI})
  target_link_libraries(interaction_tests PRIVATE presentation)

  if(WIN32)
    target_link_libraries(interaction_tests PRIVATE gdi32 user32 msimg32 dwmapi)
  else()
    target_link_libraries(interaction_tests PRIVATE PkgConfig::UI)
  endif()

  add_test(NAME interaction_tests COMMAND interaction_tests)
  set_tests_properties(interaction_tests PROPERTIES TIMEOUT 15)

  foreach(target core_tests browse_tests player_tests artwork_tests artwork_store_tests engine_tests interaction_tests)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endforeach()
endif()
