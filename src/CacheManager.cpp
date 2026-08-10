#include "CacheManager.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

namespace
{
constexpr std::uintmax_t kAnimeArtworkMaxBytes = 256ull * 1024ull * 1024ull;
constexpr auto kAnimeArtworkMaxAge = std::chrono::hours(24 * 120);

struct CacheFile
{
    std::filesystem::path path;
    std::filesystem::file_time_type modified;
    std::uintmax_t bytes = 0;
};

std::filesystem::path EnvironmentDirectory(const wchar_t* name)
{
    std::array<wchar_t, 32768> buffer {};
    const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= static_cast<DWORD>(buffer.size()))
    {
        return {};
    }
    return std::filesystem::path(buffer.data());
}

std::filesystem::path WindowsTemporaryDirectory()
{
    std::array<wchar_t, 32768> buffer {};
    const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length > 0 && length < static_cast<DWORD>(buffer.size()))
    {
        return std::filesystem::path(buffer.data());
    }

    std::error_code error;
    const std::filesystem::path fallback = std::filesystem::temp_directory_path(error);
    return error ? std::filesystem::path {} : fallback;
}

unsigned long long StableHash(const std::wstring& value)
{
    unsigned long long hash = 1469598103934665603ull;
    for (wchar_t character : value)
    {
        hash ^= static_cast<unsigned long long>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uintmax_t PathSize(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status))
    {
        return 0;
    }
    if (std::filesystem::is_regular_file(status))
    {
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        return error ? 0 : size;
    }
    if (!std::filesystem::is_directory(status))
    {
        return 0;
    }

    std::uintmax_t total = 0;
    std::filesystem::recursive_directory_iterator iterator(
        path,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end)
    {
        const std::filesystem::file_status childStatus = iterator->symlink_status(error);
        if (!error && std::filesystem::is_regular_file(childStatus))
        {
            const std::uintmax_t size = iterator->file_size(error);
            if (!error && size <= std::numeric_limits<std::uintmax_t>::max() - total)
            {
                total += size;
            }
        }
        error.clear();
        iterator.increment(error);
    }
    return total;
}

CacheMaintenanceReport RemoveOwnedDirectoryContents(const std::filesystem::path& directory)
{
    CacheMaintenanceReport report;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
    {
        return report;
    }

    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end)
    {
        const std::filesystem::path path = iterator->path();
        const std::uintmax_t bytes = PathSize(path);
        const std::filesystem::file_status status = iterator->symlink_status(error);
        if (error)
        {
            ++report.errors;
            error.clear();
            iterator.increment(error);
            continue;
        }

        std::error_code removeError;
        if (std::filesystem::is_directory(status))
        {
            const std::uintmax_t removed = std::filesystem::remove_all(path, removeError);
            if (!removeError && removed > 0)
            {
                ++report.removedDirectories;
                report.reclaimedBytes += bytes;
            }
        }
        else
        {
            ++report.examinedFiles;
            if (std::filesystem::remove(path, removeError))
            {
                ++report.removedFiles;
                report.reclaimedBytes += bytes;
            }
        }
        if (removeError)
        {
            ++report.errors;
        }
        iterator.increment(error);
    }
    if (error)
    {
        ++report.errors;
    }
    return report;
}

CacheMaintenanceReport RemoveLegacyTemporaryEntries(const std::filesystem::path& temporaryDirectory)
{
    CacheMaintenanceReport report;
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        temporaryDirectory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end)
    {
        const std::wstring name = iterator->path().filename().wstring();
        const bool legacyAudio = name.rfind(L"RexToolkitAudio_", 0) == 0;
        const bool legacyCompression = name.rfind(L"RexToolkitVideoCompression_", 0) == 0;
        if (legacyAudio || legacyCompression)
        {
            const std::filesystem::path path = iterator->path();
            const std::uintmax_t bytes = PathSize(path);
            const std::filesystem::file_status status = iterator->symlink_status(error);
            std::error_code removeError;
            if (!error && std::filesystem::is_directory(status))
            {
                const std::uintmax_t removed = std::filesystem::remove_all(path, removeError);
                if (!removeError && removed > 0)
                {
                    ++report.removedDirectories;
                    report.reclaimedBytes += bytes;
                }
            }
            else if (!error)
            {
                ++report.examinedFiles;
                if (std::filesystem::remove(path, removeError))
                {
                    ++report.removedFiles;
                    report.reclaimedBytes += bytes;
                }
            }
            if (error || removeError)
            {
                ++report.errors;
                error.clear();
            }
        }
        iterator.increment(error);
    }
    if (error)
    {
        ++report.errors;
    }
    return report;
}

CacheMaintenanceReport MigrateLegacyAnimeArtwork()
{
    CacheMaintenanceReport report;
    const std::filesystem::path appData = EnvironmentDirectory(L"APPDATA");
    if (appData.empty())
    {
        return report;
    }

    const std::filesystem::path legacy = appData / L"RexsToolkit" / L"anime_covers";
    std::error_code error;
    if (!std::filesystem::is_directory(legacy, error))
    {
        return report;
    }

    const std::filesystem::path destination = CacheManager::AnimeArtworkDirectory();
    std::filesystem::create_directories(destination, error);
    if (error)
    {
        ++report.errors;
        return report;
    }

    std::filesystem::directory_iterator iterator(
        legacy,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end)
    {
        const std::filesystem::file_status status = iterator->symlink_status(error);
        if (!error && std::filesystem::is_regular_file(status))
        {
            ++report.examinedFiles;
            const std::filesystem::path source = iterator->path();
            const std::filesystem::path target = destination / (L"v1-" + source.filename().wstring());
            std::error_code targetError;
            if (std::filesystem::exists(target, targetError))
            {
                std::filesystem::remove(source, targetError);
            }
            else
            {
                std::filesystem::rename(source, target, targetError);
                if (targetError)
                {
                    targetError.clear();
                    std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, targetError);
                    if (!targetError)
                    {
                        std::filesystem::remove(source, targetError);
                    }
                }
            }
            if (targetError)
            {
                ++report.errors;
            }
        }
        error.clear();
        iterator.increment(error);
    }
    if (error)
    {
        ++report.errors;
    }

    error.clear();
    if (std::filesystem::remove(legacy, error))
    {
        ++report.removedDirectories;
    }
    return report;
}
}

void CacheMaintenanceReport::Merge(const CacheMaintenanceReport& other)
{
    examinedFiles += other.examinedFiles;
    removedFiles += other.removedFiles;
    removedDirectories += other.removedDirectories;
    reclaimedBytes += other.reclaimedBytes;
    errors += other.errors;
}

std::wstring CacheMaintenanceReport::Summary() const
{
    std::wostringstream stream;
    stream << L"Rex's Toolkit cache maintenance: examined " << examinedFiles
           << L", removed " << removedFiles << L" file(s) and "
           << removedDirectories << L" director"
           << (removedDirectories == 1 ? L"y" : L"ies")
           << L", reclaimed " << std::fixed << std::setprecision(1)
           << static_cast<double>(reclaimedBytes) / (1024.0 * 1024.0)
           << L" MB, errors " << errors << L".";
    return stream.str();
}

std::filesystem::path CacheManager::PersistentRoot()
{
    std::filesystem::path localAppData = EnvironmentDirectory(L"LOCALAPPDATA");
    if (localAppData.empty())
    {
        localAppData = EnvironmentDirectory(L"APPDATA");
    }
    return localAppData.empty()
        ? std::filesystem::path(L".") / L"cache"
        : localAppData / L"RexToolkit" / L"Cache";
}

std::filesystem::path CacheManager::AnimeArtworkDirectory()
{
    return PersistentRoot() / L"anime-artwork-v1";
}

std::filesystem::path CacheManager::TemporaryRoot()
{
    const std::filesystem::path temporary = WindowsTemporaryDirectory();
    return temporary.empty() ? std::filesystem::path {} : temporary / L"RexToolkit";
}

std::filesystem::path CacheManager::MediaEditorTemporaryRoot()
{
    const std::filesystem::path root = TemporaryRoot();
    return root.empty() ? std::filesystem::path {} : root / L"MediaEditor";
}

std::filesystem::path CacheManager::AudioAnalysisTemporaryRoot()
{
    const std::filesystem::path root = TemporaryRoot();
    return root.empty() ? std::filesystem::path {} : root / L"audio-analysis";
}

std::filesystem::path CacheManager::VideoCompressionTemporaryRoot()
{
    const std::filesystem::path root = TemporaryRoot();
    return root.empty() ? std::filesystem::path {} : root / L"video-compression";
}

std::wstring CacheManager::VersionedFileName(
    unsigned int version,
    const std::wstring& identity,
    const std::wstring& extension)
{
    std::wstring normalizedExtension = extension;
    while (!normalizedExtension.empty() && normalizedExtension.front() == L'.')
    {
        normalizedExtension.erase(normalizedExtension.begin());
    }

    std::wostringstream stream;
    stream << L"v" << version << L"-" << std::hex << StableHash(identity);
    if (!normalizedExtension.empty())
    {
        stream << L"." << normalizedExtension;
    }
    return stream.str();
}

void CacheManager::Touch(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), error);
}

CacheMaintenanceReport CacheManager::PruneDirectory(
    const std::filesystem::path& directory,
    const CachePrunePolicy& policy)
{
    CacheMaintenanceReport report;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
    {
        return report;
    }

    std::vector<CacheFile> files;
    std::uintmax_t totalBytes = 0;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end)
    {
        const std::filesystem::file_status status = iterator->symlink_status(error);
        if (!error && std::filesystem::is_regular_file(status))
        {
            CacheFile file;
            file.path = iterator->path();
            file.bytes = iterator->file_size(error);
            if (!error)
            {
                file.modified = iterator->last_write_time(error);
            }
            if (!error)
            {
                ++report.examinedFiles;
                files.push_back(file);
                if (file.bytes <= std::numeric_limits<std::uintmax_t>::max() - totalBytes)
                {
                    totalBytes += file.bytes;
                }
            }
        }
        if (error)
        {
            ++report.errors;
            error.clear();
        }
        iterator.increment(error);
    }
    if (error)
    {
        ++report.errors;
    }

    const auto now = std::filesystem::file_time_type::clock::now();
    for (CacheFile& file : files)
    {
        if (now - file.modified <= policy.maxAge)
        {
            continue;
        }
        std::error_code removeError;
        if (std::filesystem::remove(file.path, removeError))
        {
            ++report.removedFiles;
            report.reclaimedBytes += file.bytes;
            totalBytes = file.bytes <= totalBytes ? totalBytes - file.bytes : 0;
            file.bytes = 0;
        }
        else if (removeError)
        {
            ++report.errors;
        }
    }

    std::sort(files.begin(), files.end(), [](const CacheFile& left, const CacheFile& right)
    {
        return left.modified < right.modified;
    });
    for (CacheFile& file : files)
    {
        if (totalBytes <= policy.maxBytes)
        {
            break;
        }
        if (file.bytes == 0)
        {
            continue;
        }
        std::error_code removeError;
        if (std::filesystem::remove(file.path, removeError))
        {
            ++report.removedFiles;
            report.reclaimedBytes += file.bytes;
            totalBytes = file.bytes <= totalBytes ? totalBytes - file.bytes : 0;
            file.bytes = 0;
        }
        else if (removeError)
        {
            ++report.errors;
        }
    }
    return report;
}

CacheMaintenanceReport CacheManager::PerformStartupMaintenance()
{
    CacheMaintenanceReport report;
    report.Merge(MigrateLegacyAnimeArtwork());

    CachePrunePolicy artworkPolicy;
    artworkPolicy.maxAge = kAnimeArtworkMaxAge;
    artworkPolicy.maxBytes = kAnimeArtworkMaxBytes;
    report.Merge(PruneDirectory(AnimeArtworkDirectory(), artworkPolicy));

    report.Merge(RemoveOwnedDirectoryContents(MediaEditorTemporaryRoot()));
    report.Merge(RemoveOwnedDirectoryContents(AudioAnalysisTemporaryRoot()));
    report.Merge(RemoveOwnedDirectoryContents(VideoCompressionTemporaryRoot()));

    const std::filesystem::path windowsTemp = WindowsTemporaryDirectory();
    if (!windowsTemp.empty())
    {
        report.Merge(RemoveLegacyTemporaryEntries(windowsTemp));
    }
    return report;
}
