#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::equalizer
{
struct BundledEqualizerPackageStatus
{
    std::filesystem::path installerPath;
    std::wstring version;
    std::wstring sha256;
    std::uintmax_t sizeBytes = 0;
    bool present = false;
    bool verified = false;
    std::wstring message;
};

class EqualizerApoSetup
{
public:
    static BundledEqualizerPackageStatus InspectBundledPackage();
    static bool LaunchElevatedManagedSetup(
        HWND owner,
        const std::wstring& endpointGuid,
        HANDLE& process,
        std::wstring& errorMessage);
    static bool LaunchElevatedOutputSelection(
        HWND owner,
        const std::wstring& endpointGuid,
        HANDLE& process,
        std::wstring& errorMessage);
    static bool LaunchElevatedIntegration(HWND owner, HANDLE& process, std::wstring& errorMessage);
    static int RunElevatedManagedSetup(
        const std::wstring& endpointGuid,
        std::wstring& errorMessage);
    static int RunElevatedOutputSelection(
        const std::wstring& endpointGuid,
        std::wstring& errorMessage);

    static bool IsSetupPending();
    static bool SetSetupPending(bool pending, std::wstring& errorMessage);
    static std::filesystem::path BundledInstallerPath();
    static std::wstring BundledVersion();
};
}
