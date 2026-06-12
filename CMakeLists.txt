cmake_minimum_required(VERSION 3.20)

project(RexToolkit LANGUAGES CXX)

add_executable(RexToolkit WIN32
    resources.rc
    src/AppConstants.h
    src/FileConversionService.cpp
    src/FileConversionService.h
    src/main.cpp
    src/MediaDownloadService.cpp
    src/MediaDownloadService.h
    src/ToolkitApp.cpp
    src/ToolkitApp.h
    src/UpdateChecker.cpp
    src/UpdateChecker.h
    src/resource.h
)

target_compile_features(RexToolkit PRIVATE cxx_std_17)
target_compile_definitions(RexToolkit PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)

target_link_libraries(RexToolkit PRIVATE
    Comdlg32
    Dwmapi
    Gdiplus
    Ole32
    Shell32
    UxTheme
    Windowscodecs
    Winhttp
    Winmm
)

set(BUNDLED_YT_DLP "${CMAKE_CURRENT_SOURCE_DIR}/tools/yt-dlp.exe")
if (EXISTS "${BUNDLED_YT_DLP}")
    add_custom_command(TARGET RexToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RexToolkit>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BUNDLED_YT_DLP}"
            "$<TARGET_FILE_DIR:RexToolkit>/tools/yt-dlp.exe"
        COMMENT "Copying bundled yt-dlp"
    )
endif()

set(BUNDLED_FFMPEG "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg.exe")
if (EXISTS "${BUNDLED_FFMPEG}")
    add_custom_command(TARGET RexToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RexToolkit>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BUNDLED_FFMPEG}"
            "$<TARGET_FILE_DIR:RexToolkit>/tools/ffmpeg.exe"
        COMMENT "Copying bundled FFmpeg"
    )
endif()

if (MSVC)
    target_compile_options(RexToolkit PRIVATE /W4 /permissive-)
endif()
