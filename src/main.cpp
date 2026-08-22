#include "ToolkitApp.h"

#include "EqualizerSetup.h"

#include <shellapi.h>
#include <windows.h>

#include <cwchar>
#include <utility>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    bool startMinimizedToTray = false;
    bool installEqualizerConfiguration = false;
    bool installEqualizerBackend = false;
    bool selectEqualizerOutput = false;
    bool verifyEqualizerPackage = false;
    std::wstring equalizerOutputGuid;
    std::filesystem::path editPath;
    int argumentCount = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments)
    {
        for (int index = 1; index < argumentCount; ++index)
        {
            if (wcscmp(arguments[index], L"--minimized-to-tray") == 0)
            {
                startMinimizedToTray = true;
            }
            else if (wcscmp(arguments[index], L"--equalizer-install-config") == 0)
            {
                installEqualizerConfiguration = true;
            }
            else if (wcscmp(arguments[index], L"--equalizer-install-backend") == 0)
            {
                installEqualizerBackend = true;
            }
            else if (wcscmp(arguments[index], L"--equalizer-select-output") == 0)
            {
                selectEqualizerOutput = true;
            }
            else if (wcscmp(arguments[index], L"--equalizer-output-guid") == 0 &&
                     index + 1 < argumentCount)
            {
                equalizerOutputGuid = arguments[++index];
            }
            else if (wcscmp(arguments[index], L"--equalizer-verify-package") == 0)
            {
                verifyEqualizerPackage = true;
            }
            else if (wcscmp(arguments[index], L"--edit") == 0 && index + 1 < argumentCount)
            {
                editPath = arguments[++index];
            }
        }
        LocalFree(arguments);
    }

    if (verifyEqualizerPackage)
    {
        const auto package = rex::equalizer::EqualizerApoSetup::InspectBundledPackage();
        return package.verified ? 0 : 24;
    }

    if (installEqualizerBackend)
    {
        std::wstring errorMessage;
        const int result = rex::equalizer::EqualizerApoSetup::RunElevatedManagedSetup(
            equalizerOutputGuid, errorMessage);
        if (result != 0)
        {
            MessageBoxW(
                nullptr,
                (errorMessage.empty() ? L"Equalizer setup did not complete." : errorMessage).c_str(),
                L"Equalizer Setup Failed",
                MB_TOPMOST | MB_OK | MB_ICONERROR);
        }
        return result;
    }

    if (selectEqualizerOutput)
    {
        std::wstring errorMessage;
        const int result = rex::equalizer::EqualizerApoSetup::RunElevatedOutputSelection(
            equalizerOutputGuid, errorMessage);
        if (result != 0)
        {
            MessageBoxW(
                nullptr,
                (errorMessage.empty() ? L"Equalizer output setup did not complete." : errorMessage).c_str(),
                L"Equalizer Output Setup Failed",
                MB_TOPMOST | MB_OK | MB_ICONERROR);
        }
        return result;
    }

    if (installEqualizerConfiguration)
    {
        std::wstring errorMessage;
        const bool installed =
            rex::equalizer::EqualizerApoBackend::InstallManagedIncludeForDetectedInstallation(
                errorMessage);
        if (!installed)
        {
            MessageBoxW(
                nullptr,
                (errorMessage.empty() ? L"Rex's Toolkit could not configure Equalizer APO." : errorMessage).c_str(),
                L"Equalizer Setup Failed",
                MB_TOPMOST | MB_OK | MB_ICONERROR);
        }
        return installed ? 0 : 1;
    }

    if (!editPath.empty())
    {
        startMinimizedToTray = false;
    }

    if (ToolkitApp::ActivateExistingInstanceIfRunning(
        !startMinimizedToTray || !editPath.empty(),
        editPath))
    {
        return 0;
    }

    ToolkitApp app(instance);
    app.SetStartMinimizedToTray(startMinimizedToTray);
    app.SetPendingEditPath(std::move(editPath));
    return app.Run(showCommand);
}
