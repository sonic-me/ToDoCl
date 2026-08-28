#include "TaskTicket.h"

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                       LPWSTR /*lpCmdLine*/, int /*nCmdShow*/)
{
    // ---- Single instance enforcement ----
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"TaskTicketMutex");
    if (mutex == nullptr)
    {
        // Unable to create the mutex at all - proceed anyway rather than
        // refusing to launch the app.
    }
    else if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // Another instance is already running - bring it to the front
        // instead of creating a second widget.
        HWND existing = FindWindowW(L"TaskTicketWindowClass", nullptr);
        if (existing)
        {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return 0;
    }

    TaskTicketApp app(hInstance);
    if (!app.Create())
    {
        MessageBoxW(nullptr, L"Failed to create the TaskTicket window.",
                    L"Task Ticket", MB_OK | MB_ICONERROR);
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    int result = app.RunMessageLoop();

    if (mutex)
    {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }

    return result;
}
