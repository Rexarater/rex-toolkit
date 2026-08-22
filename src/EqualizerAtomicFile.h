#pragma once

#include <windows.h>

#include <filesystem>
#include <system_error>

namespace rex::equalizer::detail
{
inline void RemoveTemporaryFile(const std::filesystem::path& temporary) noexcept
{
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
}

inline bool IsTransientCommitError(DWORD errorCode) noexcept
{
    return errorCode == ERROR_ACCESS_DENIED ||
        errorCode == ERROR_SHARING_VIOLATION ||
        errorCode == ERROR_LOCK_VIOLATION;
}

inline bool CommitTemporaryFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    DWORD& errorCode) noexcept
{
    // Equalizer APO briefly opens managed configuration files without delete
    // sharing while it reloads them. Retry only those transient lock failures;
    // permanent path and permission errors still fail without altering the
    // previous known-good destination.
    constexpr DWORD retryDelaysMs[] { 1, 2, 4, 8, 16, 32 };
    for (size_t attempt = 0;; ++attempt)
    {
        if (MoveFileExW(
                temporary.c_str(),
                destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            errorCode = ERROR_SUCCESS;
            return true;
        }

        errorCode = GetLastError();
        if (!IsTransientCommitError(errorCode) || attempt >= std::size(retryDelaysMs)) break;
        Sleep(retryDelaysMs[attempt]);
    }

    RemoveTemporaryFile(temporary);
    return false;
}
}
