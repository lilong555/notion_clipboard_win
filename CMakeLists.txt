cmake_minimum_required(VERSION 3.20)
project(notion_clipboard_win LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "notion_clipboard_win 是 Windows 原生程序，请在 Windows/MSVC 或 MinGW 环境中构建。")
endif()

enable_language(RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NOTION_CLIPBOARD_WIN_GUI "构建为无控制台窗口的后台程序" OFF)

set(APP_ICON_PATH_RC "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico")
set(RESOURCE_HEADER_PATH_RC "${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h")
configure_file(src/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)

add_executable(notion_clipboard_win
    src/main.cpp
    src/app_icon.cpp
    src/config.cpp
    src/hotkey.cpp
    src/http_client.cpp
    src/json.cpp
    src/logger.cpp
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
