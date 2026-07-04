#include "ToolkitApp.h"

#include <shellapi.h>
#include <windows.h>

#include <cwchar>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    bool startMinimizedToTray = false;
    int argumentCount = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments)
    {
        for (int index = 1; index < argumentCount; ++index)
        {
            if (wcscmp(arguments[index], L"--minimized-to-tray") == 0)
            {
                startMinimizedToTray = true;
                break;
            }
        }
        LocalFree(arguments);
    }

    if (ToolkitApp::ActivateExistingInstanceIfRunning(!startMinimizedToTray))
    {
        return 0;
    }

    ToolkitApp app(instance);
    app.SetStartMinimizedToTray(startMinimizedToTray);
    return app.Run(showCommand);
}
