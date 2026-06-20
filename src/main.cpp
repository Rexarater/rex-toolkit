#include "ToolkitApp.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (ToolkitApp::ActivateExistingInstanceIfRunning())
    {
        return 0;
    }

    ToolkitApp app(instance);
    return app.Run(showCommand);
}
