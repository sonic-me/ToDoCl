#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <climits>

//===========================================================================
// Data model
//===========================================================================

struct Task
{
    int          id = 0;
    std::wstring text;
    bool         checked   = false;
    bool         recurring = false;
};

// Screen-space (client coordinates, in *pixels already scaled for DPI*)
// rectangles for a single rendered task row. Rebuilt every time the layout
// changes (task added/removed, window resized, DPI changed).
struct TaskRowLayout
{
    RECT row;         // whole row (used for hover highlight + right-click)
    RECT checkbox;     // the small square checkbox hit-region
};

//===========================================================================
// Persisted application state (tasks.json under %APPDATA%\TaskTicket\)
//===========================================================================

struct PersistedState
{
    std::wstring     lastDate;      // "YYYY-MM-DD", local calendar date
    std::vector<Task> tasks;
    int              windowX = INT_MIN;   // INT_MIN => "no saved position"
    int              windowY = INT_MIN;
    int              nextId  = 1;         // monotonically increasing task id
};

// Returns "%APPDATA%\TaskTicket" (creating the directory if necessary).
// Returns an empty string on failure.
std::wstring GetAppDataDirectory();

// Full path to the tasks.json file.
std::wstring GetTasksFilePath();

// Loads persisted state from disk. On any failure (missing file, corrupt
// JSON, etc.) returns a valid, empty default state - it never throws and
// never crashes the app.
PersistedState LoadState();

// Atomically writes state to disk (write to temp file + rename).
// Returns true on success.
bool SaveState(const PersistedState& state);

// "YYYY-MM-DD" for the current local date.
std::wstring GetTodayDateString();

//===========================================================================
// Layout constants (logical / 96-DPI pixels - scaled at runtime by the
// current monitor's DPI factor)
//===========================================================================

namespace Layout
{
    constexpr int WindowWidth        = 430;

    constexpr int HeaderHeight       = 60;
    constexpr int DividerThickness   = 1;

    constexpr int ClockAreaHeight    = 46;   // space reserved for the clock line
    constexpr int GapAfterClock      = 22;   // clock -> first task
    constexpr int TaskRowHeight      = 38;
    constexpr int BottomPadding      = 34;   // empty "click to add" area

    constexpr int SideMargin         = 22;
    constexpr int CheckboxSize       = 20;
    constexpr int CheckboxTextGap    = 12;

    constexpr int TitleLeftMargin    = 22;
    constexpr int ButtonSpacing      = 14;
    constexpr int PinButtonSize      = 36;
    constexpr int DotDiameter        = 20;
    constexpr int HeaderRightMargin  = 20;

    constexpr int MinWindowHeight    = HeaderHeight + DividerThickness +
                                        ClockAreaHeight + GapAfterClock +
                                        BottomPadding;
}

//===========================================================================
// Application object - owns the window, the in-memory task list and all
// interactive state. One instance per process (single-instance enforced
// via a named mutex in WinMain).
//===========================================================================

class TaskTicketApp
{
public:
    explicit TaskTicketApp(HINSTANCE hInstance);
    ~TaskTicketApp();

    bool Create();
    int  RunMessageLoop();

    // Exposed only so the inline edit control's subclass procedure (a
    // free function, since Win32 subclassing requires a plain WNDPROC)
    // can forward Enter/Escape/focus-loss back into the app.
    void OnEditCommitPublic() { OnEditCommit(); }
    void OnEditCancelPublic() { OnEditCancel(); }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Message handlers
    void OnCreate(HWND hwnd);
    void OnDestroy();
    void OnPaint(HWND hwnd);
    void OnSize();
    void OnDpiChanged(WPARAM wParam, LPARAM lParam);
    void OnTimer();
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnRButtonUp(int x, int y);
    void OnKeyDown(WPARAM key);
    void OnEditCommit();
    void OnEditCancel();

    // Layout / rendering
    void RecalcLayout();
    void ResizeWindowToContent();
    int  ContentHeight() const;
    void Render(HDC hdc, const RECT& clientRect);
    void EnsureFonts();
    void ReleaseFonts();
    int  ScaleForDpi(int value) const;

    // Task operations (each persists state afterwards)
    void AddTask(const std::wstring& text);
    void DeleteTask(int index);
    void ToggleTaskChecked(int index);
    void ToggleTaskRecurring(int index);
    void PerformDailyResetIfNeeded(bool forceInvalidateAll);

    // Visual / behavioural toggles
    void TogglePinned();
    void TogglePositionLocked();
    void ApplyTopmost();
    void ApplyAcrylicBackground();
    void ApplyRoundedCorners();

    // Hit testing helpers (all in client coordinates)
    bool HitTestPin(int x, int y) const;
    bool HitTestClose(int x, int y) const;
    bool HitTestMinimize(int x, int y) const;
    bool HitTestHeaderDraggable(int x, int y) const;
    int  HitTestTaskRow(int x, int y) const;      // -1 if none
    bool HitTestCheckbox(int x, int y, int taskIndex) const;
    bool HitTestAddArea(int x, int y) const;

    // Inline "add task" edit control
    void BeginAddTask();
    void PositionAddTaskEdit();

    void Save();
    void ShowTaskContextMenu(int taskIndex, POINT screenPt);
    void ShowHeaderContextMenu(POINT screenPt);

private:
    HINSTANCE m_hInstance = nullptr;
    HWND      m_hwnd = nullptr;
    HWND      m_hwndEdit = nullptr; // inline add-task edit box, created on demand

    PersistedState m_state;
    std::vector<TaskRowLayout> m_rowLayout;

    bool m_pinned = false;          // topmost or not (default: unpinned)
    bool m_positionLocked = false;  // drag-to-move enabled/disabled

    int m_hoverIndex = -1;          // -1 = none, -2 = add area
    int m_selectedIndex = -1;
    bool m_hoverPin = false;
    bool m_hoverClose = false;
    bool m_hoverMinimize = false;

    UINT m_dpi = 96;
    HFONT m_fontTitle = nullptr;
    HFONT m_fontTask = nullptr;
    HFONT m_fontClock = nullptr;

    bool m_compositionAvailable = false;

    static constexpr UINT ID_TIMER_CLOCK = 1;
    static constexpr UINT_PTR IDC_EDIT_ADDTASK = 501;

    static constexpr UINT_PTR MENU_DELETE           = 1001;
    static constexpr UINT_PTR MENU_TOGGLE_RECURRING = 1002;
    static constexpr UINT_PTR MENU_TOGGLE_LOCK      = 1003;
};
