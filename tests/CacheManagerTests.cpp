#include "CacheManager.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace
{
bool Check(bool condition, const wchar_t* message)
{
    if (!condition)
    {
        std::wcerr << L"FAILED: " << message << L'\n';
    }
    return condition;
}

void WriteFile(const std::filesystem::path& path, std::size_t bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    const std::string data(bytes, 'x');
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
}
}

int wmain()
{
    bool passed = true;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"RexToolkitCacheTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (!Check(!error, L"temporary test directory should be created"))
    {
        return 1;
    }

    const std::wstring firstName = CacheManager::VersionedFileName(1, L"https://example.test/a", L".img");
    const std::wstring repeatedName = CacheManager::VersionedFileName(1, L"https://example.test/a", L"img");
    const std::wstring nextVersionName = CacheManager::VersionedFileName(2, L"https://example.test/a", L"img");
    passed &= Check(firstName == repeatedName, L"cache file names should be deterministic");
    passed &= Check(firstName != nextVersionName, L"cache versions should produce distinct file names");

    const std::filesystem::path ageDirectory = root / L"age";
    std::filesystem::create_directories(ageDirectory, error);
    const std::filesystem::path oldFile = ageDirectory / L"old.bin";
    const std::filesystem::path newFile = ageDirectory / L"new.bin";
    WriteFile(oldFile, 16);
    WriteFile(newFile, 16);
    std::filesystem::last_write_time(
        oldFile,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(72),
        error);

    CachePrunePolicy agePolicy;
    agePolicy.maxAge = std::chrono::hours(24);
    agePolicy.maxBytes = std::numeric_limits<std::uintmax_t>::max();
    const CacheMaintenanceReport ageReport = CacheManager::PruneDirectory(ageDirectory, agePolicy);
    passed &= Check(!std::filesystem::exists(oldFile), L"expired cache files should be removed");
    passed &= Check(std::filesystem::exists(newFile), L"fresh cache files should be kept");
    passed &= Check(ageReport.removedFiles == 1, L"age pruning should report one removed file");

    const std::filesystem::path sizeDirectory = root / L"size";
    std::filesystem::create_directories(sizeDirectory, error);
    const std::filesystem::path olderFile = sizeDirectory / L"older.bin";
    const std::filesystem::path newerFile = sizeDirectory / L"newer.bin";
    WriteFile(olderFile, 40);
    WriteFile(newerFile, 40);
    std::filesystem::last_write_time(
        olderFile,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(2),
        error);
    std::filesystem::last_write_time(
        newerFile,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(1),
        error);

    CachePrunePolicy sizePolicy;
    sizePolicy.maxAge = std::chrono::hours(24 * 365);
    sizePolicy.maxBytes = 40;
    const CacheMaintenanceReport sizeReport = CacheManager::PruneDirectory(sizeDirectory, sizePolicy);
    passed &= Check(!std::filesystem::exists(olderFile), L"size pruning should remove the oldest file first");
    passed &= Check(std::filesystem::exists(newerFile), L"size pruning should retain the newest file");
    passed &= Check(sizeReport.reclaimedBytes == 40, L"size pruning should report reclaimed bytes");
    const std::filesystem::path isolatedLocalAppData = root / L"local";
    const std::filesystem::path isolatedAppData = root / L"roaming";
    const std::filesystem::path isolatedTemp = root / L"temp";
    std::filesystem::create_directories(isolatedLocalAppData, error);
    std::filesystem::create_directories(isolatedAppData, error);
    std::filesystem::create_directories(isolatedTemp, error);
    const std::wstring localAppDataText = isolatedLocalAppData.wstring();
    const std::wstring appDataText = isolatedAppData.wstring();
    const std::wstring tempText = isolatedTemp.wstring();
    passed &= Check(SetEnvironmentVariableW(L"LOCALAPPDATA", localAppDataText.c_str()) != FALSE,
        L"isolated LOCALAPPDATA should be configured");
    passed &= Check(SetEnvironmentVariableW(L"APPDATA", appDataText.c_str()) != FALSE,
        L"isolated APPDATA should be configured");
    passed &= Check(SetEnvironmentVariableW(L"TEMP", tempText.c_str()) != FALSE,
        L"isolated TEMP should be configured");
    passed &= Check(SetEnvironmentVariableW(L"TMP", tempText.c_str()) != FALSE,
        L"isolated TMP should be configured");

    const std::wstring artworkIdentity = L"https://example.test/legacy-cover";
    const std::wstring artworkName = CacheManager::VersionedFileName(1, artworkIdentity, L"img");
    const std::filesystem::path legacyArtworkDirectory =
        isolatedAppData / L"RexsToolkit" / L"anime_covers";
    std::filesystem::create_directories(legacyArtworkDirectory, error);
    const std::filesystem::path legacyArtwork =
        legacyArtworkDirectory / artworkName.substr(std::wstring(L"v1-").size());
    WriteFile(legacyArtwork, 24);

    const std::filesystem::path previewScratch =
        CacheManager::MediaEditorTemporaryRoot() / L"preview-stale" / L"preview.mp4";
    const std::filesystem::path audioScratch =
        CacheManager::AudioAnalysisTemporaryRoot() / L"analysis-stale.wav";
    const std::filesystem::path compressionScratch =
        CacheManager::VideoCompressionTemporaryRoot() / L"job-stale" / L"pass.log";
    std::filesystem::create_directories(previewScratch.parent_path(), error);
    std::filesystem::create_directories(audioScratch.parent_path(), error);
    std::filesystem::create_directories(compressionScratch.parent_path(), error);
    WriteFile(previewScratch, 8);
    WriteFile(audioScratch, 8);
    WriteFile(compressionScratch, 8);

    const CacheMaintenanceReport startupReport = CacheManager::PerformStartupMaintenance();
    const std::filesystem::path migratedArtwork = CacheManager::AnimeArtworkDirectory() / artworkName;
    passed &= Check(std::filesystem::exists(migratedArtwork),
        L"legacy anime artwork should migrate into the versioned cache");
    passed &= Check(!std::filesystem::exists(legacyArtwork),
        L"legacy anime artwork should leave the old cache");
    passed &= Check(!std::filesystem::exists(previewScratch),
        L"stale media preview files should be removed at startup");
    passed &= Check(!std::filesystem::exists(audioScratch),
        L"stale audio-analysis files should be removed at startup");
    passed &= Check(!std::filesystem::exists(compressionScratch),
        L"stale video-compression files should be removed at startup");
    passed &= Check(startupReport.errors == 0, L"startup cache maintenance should finish without errors");


    std::filesystem::remove_all(root, error);
    if (passed)
    {
        std::wcout << L"Cache manager tests passed.\n";
        return 0;
    }
    return 1;
}
