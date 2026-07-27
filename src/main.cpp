#include "ToolkitApp.h"

#include <shellapi.h>
#include <windows.h>

#include <cwchar>
#include <utility>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    bool startMinimizedToTray = false;
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
            else if (wcscmp(arguments[index], L"--edit") == 0 && index + 1 < argumentCount)
            {
                editPath = arguments[++index];
            }
        }
        LocalFree(arguments);
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
