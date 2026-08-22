cmake_minimum_required(VERSION 3.20)

project(RexToolkit LANGUAGES CXX)

add_executable(RexToolkit WIN32
    resources.rc
    src/AnimeTrackerService.cpp
    src/AnimeTrackerService.h
    src/CacheManager.cpp
    src/CacheManager.h
    src/AppConstants.h
    src/EqualizerAtomicFile.h
    src/EqualizerConfigParser.h
    src/EqualizerBackend.cpp
    src/EqualizerDsp.cpp
    src/EqualizerService.cpp
    src/EqualizerSetup.cpp
    src/EqualizerSetup.h
    src/EqualizerSetupAutomation.cpp
    src/EqualizerSetupAutomation.h
    src/EqualizerPage.cpp
    src/EqualizerPage.h
    src/EqualizerService.h
    src/FileConversionService.cpp
    src/FileConversionService.h
    src/main.cpp
    src/MediaDownloadService.cpp
    src/MediaDownloadService.h
    src/MediaEditorPage.cpp
    src/MediaEditorPage.h
    src/MacroRecorderService.cpp
    src/MediaEditorService.cpp
    src/MediaEditorService.h
    src/MacroRecorderService.h
    src/ReminderService.cpp
    src/ReminderService.h
    src/SmartFileTransferService.cpp
    src/SmartFileTransferService.h
    src/ToolkitApp.cpp
    src/ToolkitApp.h
    src/UiComponents.cpp
    src/UiComponents.h
    src/UpdateChecker.cpp
    src/UpdateChecker.h
    src/VideoCompressionService.cpp
    src/VideoCompressionService.h
    src/resource.h
)

target_compile_features(RexToolkit PRIVATE cxx_std_17)
target_compile_definitions(RexToolkit PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(RexToolkit PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel-src/deps/json/include"
)

target_link_libraries(RexToolkit PRIVATE
    Advapi32
    Comdlg32
    Mf
    Mfplat
    Mfuuid
    Msimg32
    Propsys
    Crypt32
    Dwmapi
    Gdiplus
    Iphlpapi
    Ole32
    OleAut32
    Secur32
    Shell32
    Shlwapi
    UxTheme
    UIAutomationCore
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

set(BUNDLED_FFPROBE "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffprobe.exe")
if (EXISTS "${BUNDLED_FFPROBE}")
    add_custom_command(TARGET RexToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RexToolkit>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BUNDLED_FFPROBE}"
            "$<TARGET_FILE_DIR:RexToolkit>/tools/ffprobe.exe"
        COMMENT "Copying bundled FFprobe"
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

set(BUNDLED_QUICKJS "${CMAKE_CURRENT_SOURCE_DIR}/tools/qjs.exe")
set(BUNDLED_QUICKJS_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/tools/quickjs-ng-LICENSE.txt")
if (EXISTS "${BUNDLED_QUICKJS}" AND EXISTS "${BUNDLED_QUICKJS_LICENSE}")
    add_custom_command(TARGET RexToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RexToolkit>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BUNDLED_QUICKJS}"
            "$<TARGET_FILE_DIR:RexToolkit>/tools/qjs.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BUNDLED_QUICKJS_LICENSE}"
            "$<TARGET_FILE_DIR:RexToolkit>/tools/quickjs-ng-LICENSE.txt"
        COMMENT "Copying bundled QuickJS-NG runtime and license"
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

set(BUNDLED_INTERCEPTION_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/interception")
if (EXISTS "${BUNDLED_INTERCEPTION_DIR}")
    add_custom_command(TARGET RexToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${BUNDLED_INTERCEPTION_DIR}"
            "$<TARGET_FILE_DIR:RexToolkit>/tools/interception"
        COMMENT "Copying optional Interception backend"
    )
endif()

set(BUNDLED_EQUALIZER_APO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/equalizerapo")
if (EXISTS "${BUNDLED_EQUALIZER_APO_DIR}")
    add_custom_command(TARGET RexToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${BUNDLED_EQUALIZER_APO_DIR}"
            "$<TARGET_FILE_DIR:RexToolkit>/tools/equalizerapo"
        COMMENT "Copying verified Equalizer APO setup, source, license, and provenance"
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

set(REX_THIRD_PARTY_NOTICES "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES.md")
set(REX_EQUALIZER_HELP "${CMAKE_CURRENT_SOURCE_DIR}/docs/EQUALIZER.md")
if (EXISTS "${REX_THIRD_PARTY_NOTICES}" AND EXISTS "${REX_EQUALIZER_HELP}")
    add_custom_command(TARGET RexToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${REX_THIRD_PARTY_NOTICES}"
            "$<TARGET_FILE_DIR:RexToolkit>/THIRD_PARTY_LICENSES.md"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RexToolkit>/docs"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${REX_EQUALIZER_HELP}"
            "$<TARGET_FILE_DIR:RexToolkit>/docs/EQUALIZER.md"
        COMMENT "Copying Equalizer help and third-party notices"
    )
endif()

if (MSVC)
    target_compile_options(RexToolkit PRIVATE /W4 /permissive-)
endif()

include(CTest)
if (BUILD_TESTING)
    add_executable(RexToolkitCacheTests EXCLUDE_FROM_ALL
        tests/CacheManagerTests.cpp
        src/CacheManager.cpp
        src/CacheManager.h
    )
    target_compile_features(RexToolkitCacheTests PRIVATE cxx_std_17)
    target_compile_definitions(RexToolkitCacheTests PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
    target_include_directories(RexToolkitCacheTests PRIVATE src)
    if (MSVC)
        target_compile_options(RexToolkitCacheTests PRIVATE /W4 /permissive-)
    endif()
    add_test(NAME RexToolkitCacheTests COMMAND RexToolkitCacheTests)

    add_executable(RexToolkitEqualizerTests EXCLUDE_FROM_ALL
        tests/EqualizerTests.cpp
        src/EqualizerAtomicFile.h
        src/EqualizerConfigParser.h
        src/EqualizerDsp.cpp
        src/EqualizerSetupAutomation.cpp
        src/EqualizerSetupAutomation.h
        src/EqualizerService.h
    )
    target_compile_features(RexToolkitEqualizerTests PRIVATE cxx_std_17)
    target_compile_definitions(RexToolkitEqualizerTests PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
    target_include_directories(RexToolkitEqualizerTests PRIVATE
        src
        "third_party/libdatachannel-src/deps/json/include"
    )
    target_link_libraries(RexToolkitEqualizerTests PRIVATE Ole32 UIAutomationCore Winhttp)
    if (MSVC)
        target_compile_options(RexToolkitEqualizerTests PRIVATE /W4 /permissive-)
    endif()
    add_test(NAME RexToolkitEqualizerTests COMMAND RexToolkitEqualizerTests)
    add_test(
        NAME RexToolkitEqualizerPackageSmoke
        COMMAND $<TARGET_FILE:RexToolkit> --equalizer-verify-package
    )

    add_executable(RexToolkitUiComponentsTests EXCLUDE_FROM_ALL
        tests/UiComponentsTests.cpp
        src/UiComponents.cpp
        src/UiComponents.h
    )
    target_compile_features(RexToolkitUiComponentsTests PRIVATE cxx_std_17)
    target_compile_definitions(RexToolkitUiComponentsTests PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
    target_include_directories(RexToolkitUiComponentsTests PRIVATE src)
    target_link_libraries(RexToolkitUiComponentsTests PRIVATE Gdiplus)
    if (MSVC)
        target_compile_options(RexToolkitUiComponentsTests PRIVATE /W4 /permissive-)
    endif()
    add_test(NAME RexToolkitUiComponentsTests COMMAND RexToolkitUiComponentsTests)
endif()
