cmake_minimum_required(VERSION 3.20)

project(RexToolkit LANGUAGES CXX)

add_executable(RexToolkit WIN32
    resources.rc
    src/AnimeTrackerService.cpp
    src/AnimeTrackerService.h
    src/AppConstants.h
    src/FileConversionService.cpp
    src/FileConversionService.h
    src/main.cpp
    src/MediaDownloadService.cpp
    src/MediaDownloadService.h
    src/ReminderService.cpp
    src/ReminderService.h
    src/SmartFileTransferService.cpp
    src/SmartFileTransferService.h
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
    Crypt32
    Dwmapi
    Gdiplus
    Iphlpapi
    Ole32
    OleAut32
    Secur32
    Shell32
    UxTheme
    Windowscodecs
    Winhttp
    Wininet
    Winmm
    Ws2_32
    Bcrypt
    Urlmon
)

set(LIBDATACHANNEL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel-src")
set(LIBDATACHANNEL_BUILD "${LIBDATACHANNEL_ROOT}/build/release")
set(OPENSSL_STATIC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl-x64-windows-static" CACHE PATH "Static OpenSSL root used by libdatachannel")
set(LIBDATACHANNEL_STATIC_LIB "${LIBDATACHANNEL_BUILD}/datachannel-static.lib")
set(LIBDATACHANNEL_JUICE_LIB "${LIBDATACHANNEL_BUILD}/deps/libjuice/juice-static.lib")
set(LIBDATACHANNEL_USRSCTP_LIB "${LIBDATACHANNEL_BUILD}/deps/usrsctp/usrsctplib/usrsctp.lib")
if (EXISTS "${LIBDATACHANNEL_STATIC_LIB}" AND EXISTS "${LIBDATACHANNEL_JUICE_LIB}" AND EXISTS "${LIBDATACHANNEL_USRSCTP_LIB}" AND EXISTS "${OPENSSL_STATIC_ROOT}/lib/libssl.lib" AND EXISTS "${OPENSSL_STATIC_ROOT}/lib/libcrypto.lib")
    target_compile_definitions(RexToolkit PRIVATE REX_TOOLKIT_HAS_LIBDATACHANNEL=1 RTC_STATIC=1)
    target_include_directories(RexToolkit PRIVATE
        "${LIBDATACHANNEL_ROOT}/include"
        "${OPENSSL_STATIC_ROOT}/include"
    )
    target_link_libraries(RexToolkit PRIVATE
        "${LIBDATACHANNEL_STATIC_LIB}"
        "${LIBDATACHANNEL_JUICE_LIB}"
        "${LIBDATACHANNEL_USRSCTP_LIB}"
        "${OPENSSL_STATIC_ROOT}/lib/libssl.lib"
        "${OPENSSL_STATIC_ROOT}/lib/libcrypto.lib"
    )
endif()

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

set(BUNDLED_ESSENTIA "${CMAKE_CURRENT_SOURCE_DIR}/tools/essentia_streaming_extractor_music.exe")
if (EXISTS "${BUNDLED_ESSENTIA}")
    add_custom_command(TARGET RexToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RexToolkit>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BUNDLED_ESSENTIA}"
            "$<TARGET_FILE_DIR:RexToolkit>/tools/essentia_streaming_extractor_music.exe"
        COMMENT "Copying bundled Essentia music extractor"
    )
endif()

set(BUNDLED_ESSENTIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/essentia")
if (EXISTS "${BUNDLED_ESSENTIA_DIR}")
    add_custom_command(TARGET RexToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${BUNDLED_ESSENTIA_DIR}"
            "$<TARGET_FILE_DIR:RexToolkit>/tools/essentia"
        COMMENT "Copying bundled Essentia folder"
    )
endif()

set(REMINDER_NOTIFICATION_SOUND "${CMAKE_CURRENT_SOURCE_DIR}/assets/reminder_notification.mp3")
if (EXISTS "${REMINDER_NOTIFICATION_SOUND}")
    add_custom_command(TARGET RexToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RexToolkit>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${REMINDER_NOTIFICATION_SOUND}"
            "$<TARGET_FILE_DIR:RexToolkit>/assets/reminder_notification.mp3"
        COMMENT "Copying reminder notification sound"
    )
endif()

if (MSVC)
    target_compile_options(RexToolkit PRIVATE /W4 /permissive-)
endif()
