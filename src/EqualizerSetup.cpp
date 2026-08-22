#include "EqualizerSetup.h"

#include "EqualizerService.h"
#include "EqualizerSetupAutomation.h"

#include <bcrypt.h>
#include <shellapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

namespace rex::equalizer
{
namespace
{
constexpr wchar_t kInstallerFileName[] = L"EqualizerAPO-x64-1.4.2.exe";
constexpr wchar_t kInstallerVersion[] = L"1.4.2";
constexpr wchar_t kInstallerSha256[] =
    L"7403BE7427BBE1936A40DDED082829B6E217FC4F5990FEE5CBA501F0AE055AFA";
constexpr std::uintmax_t kInstallerSize = 11980366;
constexpr wchar_t kPendingMarkerFileName[] = L"backend_setup.pending";

std::filesystem::path ExecutableDirectory()
{
    std::array<wchar_t, 32768> path {};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return std::filesystem::path(path.data()).parent_path();
}

std::wstring WindowsError(DWORD error)
{
    wchar_t* rawMessage = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&rawMessage), 0, nullptr);
    std::wstring message = length && rawMessage
        ? std::wstring(rawMessage, length)
        : L"Windows error " + std::to_wstring(error);
    if (rawMessage) LocalFree(rawMessage);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
    {
        message.pop_back();
    }
    return message;
}

bool Sha256File(const std::filesystem::path& path, std::wstring& digest, std::wstring& errorMessage)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> hashObject;
    std::array<unsigned char, 32> hashBytes {};
    bool success = false;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    {
        errorMessage = L"Windows could not initialize SHA-256 validation.";
        return false;
    }

    DWORD objectLength = 0;
    DWORD resultLength = 0;
    if (BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength), &resultLength, 0) < 0 || objectLength == 0)
    {
        errorMessage = L"Windows could not prepare SHA-256 validation.";
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    hashObject.resize(objectLength);
    if (BCryptCreateHash(
            algorithm, &hash, hashObject.data(), static_cast<ULONG>(hashObject.size()),
            nullptr, 0, 0) < 0)
    {
        errorMessage = L"Windows could not create the SHA-256 validator.";
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        errorMessage = L"The bundled Equalizer APO installer could not be opened.";
    }
    else
    {
        std::array<char, 64 * 1024> buffer {};
        success = true;
        while (input)
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count <= 0) break;
            if (BCryptHashData(
                    hash, reinterpret_cast<PUCHAR>(buffer.data()),
                    static_cast<ULONG>(count), 0) < 0)
            {
                success = false;
                errorMessage = L"The bundled Equalizer APO installer could not be validated.";
                break;
            }
        }
        if (input.bad())
        {
            success = false;
            errorMessage = L"The bundled Equalizer APO installer could not be read completely.";
        }
        if (success && BCryptFinishHash(
                hash, hashBytes.data(), static_cast<ULONG>(hashBytes.size()), 0) < 0)
        {
            success = false;
            errorMessage = L"The bundled Equalizer APO installer hash could not be completed.";
        }
    }

    if (success)
    {
        std::wostringstream text;
        text << std::uppercase << std::hex << std::setfill(L'0');
        for (const unsigned char byte : hashBytes)
        {
            text << std::setw(2) << static_cast<unsigned int>(byte);
        }
        digest = text.str();
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

bool LaunchElevatedHelper(
    HWND owner,
    const wchar_t* arguments,
    HANDLE& process,
    std::wstring& errorMessage)
{
    process = nullptr;
    std::array<wchar_t, 32768> executable {};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
    {
        errorMessage = L"Rex's Toolkit could not locate its setup helper.";
        return false;
    }

    const std::filesystem::path workingDirectory =
        std::filesystem::path(executable.data()).parent_path();
    SHELLEXECUTEINFOW launch {};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    launch.hwnd = owner;
    launch.lpVerb = L"runas";
    launch.lpFile = executable.data();
    launch.lpParameters = arguments;
    launch.lpDirectory = workingDirectory.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch) || !launch.hProcess)
    {
        const DWORD error = GetLastError();
        errorMessage = error == ERROR_CANCELLED
            ? L"Equalizer setup was canceled before Windows made any changes."
            : L"Windows could not start Equalizer setup. " + WindowsError(error);
        return false;
    }

    process = launch.hProcess;
    return true;
}

bool StartInstallerAndWait(
    const std::filesystem::path& installer,
    const setup::SelectorDeviceIdentity& target,
    DWORD& exitCode,
    std::wstring& errorMessage)
{
    std::wstring commandLine = L"\"" + installer.wstring() + L"\" /S";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(
            installer.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
            nullptr, installer.parent_path().c_str(), &startup, &process))
    {
        errorMessage = L"Windows could not start the bundled Equalizer APO installer. " +
            WindowsError(GetLastError());
        return false;
    }

    CloseHandle(process.hThread);
    std::atomic_bool stopAutomation { false };
    std::thread automation([&]() {
        setup::MonitorDeviceSelector(target, 0, stopAutomation);
    });
    const DWORD waitResult = WaitForSingleObject(process.hProcess, INFINITE);
    const bool completed = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &exitCode);
    stopAutomation.store(true, std::memory_order_relaxed);
    if (automation.joinable()) automation.join();
    CloseHandle(process.hProcess);
    if (!completed)
    {
        errorMessage = L"Rex's Toolkit could not confirm that Equalizer APO setup completed.";
        return false;
    }
    return true;
}

bool StartSelectorAndWait(
    const std::filesystem::path& selector,
    const setup::SelectorDeviceIdentity& target,
    DWORD& exitCode,
    std::wstring& errorMessage)
{
    std::wstring commandLine = L"\"" + selector.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(
            selector.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
            nullptr, selector.parent_path().c_str(), &startup, &process))
    {
        errorMessage = L"Windows could not start Equalizer APO Device Selector. " +
            WindowsError(GetLastError());
        return false;
    }

    CloseHandle(process.hThread);
    std::atomic_bool stopAutomation { false };
    std::thread automation([&]() {
        setup::MonitorDeviceSelector(target, process.dwProcessId, stopAutomation);
    });
    const DWORD waitResult = WaitForSingleObject(process.hProcess, INFINITE);
    const bool completed = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &exitCode);
    stopAutomation.store(true, std::memory_order_relaxed);
    if (automation.joinable()) automation.join();
    CloseHandle(process.hProcess);
    if (!completed)
    {
        errorMessage = L"Rex's Toolkit could not confirm that output setup completed.";
        return false;
    }
    return true;
}

std::wstring TargetArguments(
    const wchar_t* command,
    const std::wstring& endpointGuid)
{
    return std::wstring(command) + L" --equalizer-output-guid \"" +
        endpointGuid + L"\"";
}

bool WaitForEndpointConfigured(const std::wstring& endpointGuid)
{
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        if (setup::IsEndpointConfigured(endpointGuid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(125));
    }
    return setup::IsEndpointConfigured(endpointGuid);
}

bool WaitForServiceState(
    SC_HANDLE service,
    DWORD desiredState,
    DWORD timeoutMilliseconds,
    std::wstring& errorMessage)
{
    const ULONGLONG started = GetTickCount64();
    for (;;)
    {
        SERVICE_STATUS_PROCESS status {};
        DWORD bytesNeeded = 0;
        if (!QueryServiceStatusEx(
                service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<BYTE*>(&status), sizeof(status), &bytesNeeded))
        {
            errorMessage = L"Windows could not read the audio service state. " +
                WindowsError(GetLastError());
            return false;
        }
        if (status.dwCurrentState == desiredState) return true;
        if (GetTickCount64() - started >= timeoutMilliseconds)
        {
            errorMessage = L"Windows Audio did not finish changing state in time.";
            return false;
        }
        Sleep(200);
    }
}

bool RestartWindowsAudioService(std::wstring& errorMessage)
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
    {
        errorMessage = L"Windows could not open the Service Control Manager. " +
            WindowsError(GetLastError());
        return false;
    }

    SC_HANDLE service = OpenServiceW(
        manager, L"Audiosrv", SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START);
    if (!service)
    {
        errorMessage = L"Windows could not open the Windows Audio service. " +
            WindowsError(GetLastError());
        CloseServiceHandle(manager);
        return false;
    }

    SERVICE_STATUS_PROCESS status {};
    DWORD bytesNeeded = 0;
    bool success = QueryServiceStatusEx(
        service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &bytesNeeded) != FALSE;
    if (!success)
    {
        errorMessage = L"Windows could not read the audio service state. " +
            WindowsError(GetLastError());
    }
    else if (status.dwCurrentState != SERVICE_STOPPED)
    {
        if (status.dwCurrentState != SERVICE_STOP_PENDING)
        {
            SERVICE_STATUS controlStatus {};
            if (!ControlService(service, SERVICE_CONTROL_STOP, &controlStatus) &&
                GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
            {
                errorMessage = L"Windows could not stop the audio service. " +
                    WindowsError(GetLastError());
                success = false;
            }
        }
        if (success)
        {
            success = WaitForServiceState(
                service, SERVICE_STOPPED, 20000, errorMessage);
        }
    }

    if (success && !StartServiceW(service, 0, nullptr) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
    {
        errorMessage = L"Windows could not start the audio service again. " +
            WindowsError(GetLastError());
        success = false;
    }
    if (success)
    {
        success = WaitForServiceState(
            service, SERVICE_RUNNING, 20000, errorMessage);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return success;
}
}

std::filesystem::path EqualizerApoSetup::BundledInstallerPath()
{
    return ExecutableDirectory() / L"tools" / L"equalizerapo" / kInstallerFileName;
}

std::wstring EqualizerApoSetup::BundledVersion()
{
    return kInstallerVersion;
}

BundledEqualizerPackageStatus EqualizerApoSetup::InspectBundledPackage()
{
    BundledEqualizerPackageStatus status;
    status.installerPath = BundledInstallerPath();
    status.version = kInstallerVersion;
    status.sha256 = kInstallerSha256;

    std::error_code fileError;
    status.present = std::filesystem::is_regular_file(status.installerPath, fileError);
    if (!status.present || fileError)
    {
        status.message = L"The bundled Equalizer APO setup package is missing. Restore or reinstall the complete Rex's Toolkit package.";
        return status;
    }

    status.sizeBytes = std::filesystem::file_size(status.installerPath, fileError);
    if (fileError || status.sizeBytes != kInstallerSize)
    {
        status.message = L"The bundled Equalizer APO setup package is incomplete or has been changed.";
        return status;
    }

    std::wstring actualHash;
    if (!Sha256File(status.installerPath, actualHash, status.message)) return status;
    if (actualHash != kInstallerSha256)
    {
        status.message = L"The bundled Equalizer APO setup package failed its integrity check and will not be opened.";
        return status;
    }

    status.verified = true;
    status.message = L"Bundled Equalizer APO " + status.version + L" is verified and ready to install.";
    return status;
}

bool EqualizerApoSetup::LaunchElevatedManagedSetup(
    HWND owner,
    const std::wstring& endpointGuid,
    HANDLE& process,
    std::wstring& errorMessage)
{
    const auto package = InspectBundledPackage();
    if (!package.verified)
    {
        errorMessage = package.message;
        return false;
    }

    if (!setup::IsValidEndpointGuid(endpointGuid))
    {
        errorMessage = L"Rex's Toolkit could not identify the selected Windows output.";
        return false;
    }

    if (!SetSetupPending(true, errorMessage)) return false;
    const std::wstring arguments = TargetArguments(
        L"--equalizer-install-backend", endpointGuid);
    if (!LaunchElevatedHelper(owner, arguments.c_str(), process, errorMessage))
    {
        std::wstring ignored;
        SetSetupPending(false, ignored);
        return false;
    }
    return true;
}

bool EqualizerApoSetup::LaunchElevatedOutputSelection(
    HWND owner,
    const std::wstring& endpointGuid,
    HANDLE& process,
    std::wstring& errorMessage)
{
    if (!setup::IsValidEndpointGuid(endpointGuid))
    {
        errorMessage = L"Rex's Toolkit could not identify the selected Windows output.";
        return false;
    }
    if (!SetSetupPending(true, errorMessage)) return false;
    const std::wstring arguments = TargetArguments(
        L"--equalizer-select-output", endpointGuid);
    if (!LaunchElevatedHelper(owner, arguments.c_str(), process, errorMessage))
    {
        std::wstring ignored;
        SetSetupPending(false, ignored);
        return false;
    }
    return true;
}

bool EqualizerApoSetup::LaunchElevatedIntegration(
    HWND owner,
    HANDLE& process,
    std::wstring& errorMessage)
{
    if (!SetSetupPending(true, errorMessage)) return false;
    if (!LaunchElevatedHelper(owner, L"--equalizer-install-config", process, errorMessage))
    {
        std::wstring ignored;
        SetSetupPending(false, ignored);
        return false;
    }
    return true;
}

int EqualizerApoSetup::RunElevatedManagedSetup(
    const std::wstring& endpointGuid,
    std::wstring& errorMessage)
{
    const auto package = InspectBundledPackage();
    if (!package.verified)
    {
        errorMessage = package.message;
        return 20;
    }
    if (!setup::IsValidEndpointGuid(endpointGuid))
    {
        errorMessage = L"The selected Windows output identifier is invalid.";
        return 23;
    }

    setup::SelectorDeviceIdentity target;
    target.endpointGuid = endpointGuid;
    setup::LoadSelectorDeviceIdentity(endpointGuid, target);

    std::wstring markerError;
    SetSetupPending(true, markerError);
    DWORD installerExitCode = 0;
    if (!StartInstallerAndWait(
            package.installerPath, target, installerExitCode, errorMessage))
    {
        return 21;
    }
    constexpr DWORD kSuccessRebootInitiated = 1641;
    constexpr DWORD kSuccessRebootRequired = 3010;
    constexpr DWORD kSuccessRestartRequired = 3011;
    const bool installerSucceeded =
        installerExitCode == ERROR_SUCCESS ||
        installerExitCode == kSuccessRebootInitiated ||
        installerExitCode == kSuccessRebootRequired ||
        installerExitCode == kSuccessRestartRequired;
    if (!installerSucceeded)
    {
        errorMessage = L"Equalizer APO setup did not complete (installer code " +
            std::to_wstring(installerExitCode) + L").";
        return 22;
    }

    if (!WaitForEndpointConfigured(endpointGuid))
    {
        errorMessage = L"Equalizer APO finished installing, but Rex's Toolkit could not "
            L"verify that it attached to the selected Windows output.";
        return 29;
    }

    std::wstring integrationError;
    if (!EqualizerApoBackend::InstallManagedIncludeForDetectedInstallation(integrationError))
    {
        errorMessage = integrationError.empty()
            ? L"Rex's Toolkit could not install its managed Equalizer configuration."
            : integrationError;
        return 30;
    }
    std::wstring activationError;
    if (!RestartWindowsAudioService(activationError))
    {
        errorMessage = L"Equalizer APO is installed and configured, but Rex's Toolkit could not "
            L"restart Windows Audio automatically. " + activationError +
            L" Restart Windows once to activate audio processing.";
        return 31;
    }
    errorMessage.clear();
    return 0;
}

int EqualizerApoSetup::RunElevatedOutputSelection(
    const std::wstring& endpointGuid,
    std::wstring& errorMessage)
{
    if (!setup::IsValidEndpointGuid(endpointGuid))
    {
        errorMessage = L"The selected Windows output identifier is invalid.";
        return 25;
    }
    if (!setup::IsEndpointConfigured(endpointGuid))
    {
        const std::filesystem::path selector = setup::FindInstalledDeviceSelector();
        if (selector.empty())
        {
            errorMessage = L"Equalizer APO Device Selector could not be found. Run the bundled setup again to repair the engine.";
            return 26;
        }

        setup::SelectorDeviceIdentity target;
        target.endpointGuid = endpointGuid;
        setup::LoadSelectorDeviceIdentity(endpointGuid, target);
        DWORD selectorExitCode = 0;
        if (!StartSelectorAndWait(selector, target, selectorExitCode, errorMessage)) return 27;
        if (selectorExitCode != ERROR_SUCCESS)
        {
            errorMessage = L"Equalizer APO output setup did not complete (selector code " +
                std::to_wstring(selectorExitCode) + L").";
            return 28;
        }
    }
    if (!WaitForEndpointConfigured(endpointGuid))
    {
        errorMessage = L"Equalizer APO closed without configuring the selected Windows output. "
            L"No unverified audio setup was enabled.";
        return 29;
    }

    std::wstring integrationError;
    if (!EqualizerApoBackend::InstallManagedIncludeForDetectedInstallation(integrationError))
    {
        errorMessage = integrationError.empty()
            ? L"The output was selected, but Rex's Toolkit could not install its managed Equalizer configuration."
            : integrationError;
        return 30;
    }
    std::wstring activationError;
    if (!RestartWindowsAudioService(activationError))
    {
        errorMessage = L"The output is configured, but Rex's Toolkit could not restart Windows "
            L"Audio automatically. " + activationError +
            L" Restart Windows once to activate audio processing.";
        return 31;
    }
    errorMessage.clear();
    return 0;
}

bool EqualizerApoSetup::IsSetupPending()
{
    std::error_code error;
    return std::filesystem::is_regular_file(
        EqualizerService::DefaultDataDirectory() / kPendingMarkerFileName, error);
}

bool EqualizerApoSetup::SetSetupPending(bool pending, std::wstring& errorMessage)
{
    const std::filesystem::path marker =
        EqualizerService::DefaultDataDirectory() / kPendingMarkerFileName;
    std::error_code fileError;
    if (!pending)
    {
        std::filesystem::remove(marker, fileError);
        if (fileError)
        {
            errorMessage = L"Rex's Toolkit could not clear its Equalizer setup marker.";
            return false;
        }
        errorMessage.clear();
        return true;
    }

    std::filesystem::create_directories(marker.parent_path(), fileError);
    if (fileError)
    {
        errorMessage = L"Rex's Toolkit could not create its Equalizer setup folder.";
        return false;
    }
    std::ofstream output(marker, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        errorMessage = L"Rex's Toolkit could not remember the pending Equalizer setup.";
        return false;
    }
    output << "Equalizer APO 1.4.2 setup pending\n";
    if (!output)
    {
        errorMessage = L"Rex's Toolkit could not save the pending Equalizer setup.";
        return false;
    }
    errorMessage.clear();
    return true;
}
}
