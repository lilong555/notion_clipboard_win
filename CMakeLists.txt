cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NCW_PROJECT_VERSION)
string(STRIP "${NCW_PROJECT_VERSION}" NCW_PROJECT_VERSION)

project(notion_clipboard_win VERSION "${NCW_PROJECT_VERSION}" LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "notion_clipboard_win 是 Windows 原生程序，请在 Windows/MSVC 或 MinGW 环境中构建。")
endif()

enable_language(RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NOTION_CLIPBOARD_WIN_GUI "构建为无控制台窗口的后台程序" OFF)

include(CTest)

set(APP_ICON_PATH_RC "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico")
set(RESOURCE_HEADER_PATH_RC "${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h")
set(NCW_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(NCW_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(NCW_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(NCW_VERSION_BUILD "0")
set(NCW_VERSION_STRING "${PROJECT_VERSION}")
configure_file(src/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)

add_executable(notion_clipboard_win
    src/main.cpp
    src/app_icon.cpp
    src/autostart.cpp
    src/config.cpp
    src/config_page.cpp
    src/converter.cpp
    src/hotkey.cpp
    src/http_client.cpp
    src/json.cpp
    src/logger.cpp
    src/obsidian.cpp
    src/queue.cpp
    src/upload_center.cpp
    src/upload_target.cpp
    src/util.cpp
    src/win_util.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
)

if(NOTION_CLIPBOARD_WIN_GUI)
    set_target_properties(notion_clipboard_win PROPERTIES WIN32_EXECUTABLE TRUE)
    target_compile_definitions(notion_clipboard_win PRIVATE NOTION_CLIPBOARD_WIN_GUI)
endif()

target_compile_definitions(notion_clipboard_win PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(notion_clipboard_win PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_libraries(notion_clipboard_win PRIVATE
    winhttp
    user32
    shell32
    gdi32
)

if(MSVC)
    target_compile_options(notion_clipboard_win PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(notion_clipboard_win PRIVATE -Wall -Wextra -Wpedantic)
    if(MINGW)
        target_link_options(notion_clipboard_win PRIVATE -municode)
    endif()
endif()

if(BUILD_TESTING AND NOT NOTION_CLIPBOARD_WIN_GUI)
    set(BF_OBSIDIAN_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/bf.obsidian.md")
    add_test(NAME self_test COMMAND notion_clipboard_win --self-test)
    add_test(NAME bf_dry_run
        COMMAND notion_clipboard_win --dry-run-file "${CMAKE_CURRENT_SOURCE_DIR}/test/bf.txt")
    add_test(NAME bf_obsidian_dry_run
        COMMAND notion_clipboard_win --dry-run-obsidian-file
            "${CMAKE_CURRENT_SOURCE_DIR}/test/bf.txt"
            "${BF_OBSIDIAN_OUTPUT}")
    add_test(NAME bf_obsidian_verify
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${BF_OBSIDIAN_OUTPUT}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify-obsidian-fixture.cmake")
    set_tests_properties(self_test bf_dry_run bf_obsidian_dry_run PROPERTIES TIMEOUT 120)
    set_tests_properties(bf_obsidian_verify PROPERTIES DEPENDS bf_obsidian_dry_run TIMEOUT 30)
endif()
