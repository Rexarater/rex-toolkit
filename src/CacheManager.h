#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

struct CachePrunePolicy
{
    std::chrono::hours maxAge { std::chrono::hours(24 * 90) };
    std::uintmax_t maxBytes = 256ull * 1024ull * 1024ull;
};

struct CacheMaintenanceReport
{
    std::uintmax_t examinedFiles = 0;
    std::uintmax_t removedFiles = 0;
    std::uintmax_t removedDirectories = 0;
    std::uintmax_t reclaimedBytes = 0;
    std::uintmax_t errors = 0;

    void Merge(const CacheMaintenanceReport& other);
    std::wstring Summary() const;
};

class CacheManager
{
public:
    static std::filesystem::path PersistentRoot();
    static std::filesystem::path AnimeArtworkDirectory();
    static std::filesystem::path TemporaryRoot();
    static std::filesystem::path MediaEditorTemporaryRoot();
    static std::filesystem::path AudioAnalysisTemporaryRoot();
    static std::filesystem::path VideoCompressionTemporaryRoot();

    static std::wstring VersionedFileName(
        unsigned int version,
        const std::wstring& identity,
        const std::wstring& extension);
    static void Touch(const std::filesystem::path& path);

    static CacheMaintenanceReport PruneDirectory(
        const std::filesystem::path& directory,
        const CachePrunePolicy& policy);
    static CacheMaintenanceReport PerformStartupMaintenance();
};
