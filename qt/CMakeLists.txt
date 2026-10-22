cmake_minimum_required(VERSION 3.21)
project(AnyVideoDownload VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network WebSockets)
qt_standard_project_setup()

qt_add_executable(AnyVideoDownload WIN32
    src/main.cpp
    src/settings.h src/settings.cpp
    src/engine.h src/engine.cpp
    src/mainwindow.h src/mainwindow.cpp
    src/capture.h src/capture.cpp
    src/platform.h src/platform.cpp
)
target_link_libraries(AnyVideoDownload PRIVATE Qt6::Widgets Qt6::Network Qt6::WebSockets)

if(MSVC)
    # /utf-8：原始碼含中文字串，不加的話 MSVC 會用系統字碼頁（CP950）解讀
    target_compile_options(AnyVideoDownload PRIVATE /utf-8 /W4 /permissive-)
endif()
target_compile_definitions(AnyVideoDownload PRIVATE UNICODE _UNICODE QT_NO_CAST_FROM_BYTEARRAY)

# EnsureH264 外掛（與 C# 版共用）複製到執行檔旁邊的 yt-dlp-plugins
add_custom_command(TARGET AnyVideoDownload POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../shared/yt-dlp-plugins"
            "$<TARGET_FILE_DIR:AnyVideoDownload>/yt-dlp-plugins"
    COMMENT "Copy yt-dlp plugins")
