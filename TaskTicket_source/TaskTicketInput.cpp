#include "TaskTicket.h"
#include <windowsx.h>
#include <algorithm>

//===========================================================================
// Hit testing (all in client coordinates, already DPI-scaled since the
// layout rectangles were built with ScaleForDpi()).
//===========================================================================

bool TaskTicketApp::HitTestPin(int x, int y) const
{
    int width = ScaleForDpi(Layout::WindowWidth);
    int headerH = ScaleForDpi(Layout::HeaderHeight);
    int dot = ScaleForDpi(Layout::DotDiameter);
    int pinSize = ScaleForDpi(Layout::PinButtonSize);
    int spacing = ScaleForDpi(Layout::ButtonSpacing);
    int rightMargin = ScaleForDpi(Layout::HeaderRightMargin);
    int centerY = headerH / 2;

    int yellowLeft = (width - rightMargin) - dot;
    int redLeft = yellowLeft - spacing - dot;
    int pinRight = redLeft - spacing;
    int pinLeft = pinRight - pinSize;

    RECT r = { pinLeft, centerY - pinSize / 2, pinRight, centerY + pinSize / 2 };
    POINT pt{ x, y };
    return PtInRect(&r, pt) != 0;
}

bool TaskTicketApp::HitTestClose(int x, int y) const
{
    int width = ScaleForDpi(Layout::WindowWidth);
    int headerH = ScaleForDpi(Layout::HeaderHeight);
    int dot = ScaleForDpi(Layout::DotDiameter);
    int spacing = ScaleForDpi(Layout::ButtonSpacing);
    int rightMargin = ScaleForDpi(Layout::HeaderRightMargin);
    int centerY = headerH / 2;

    int yellowLeft = (width - rightMargin) - dot;
    int redRight = yellowLeft - spacing;
    int redLeft = redRight - dot;

    RECT r = { redLeft, centerY - dot / 2, redRight, centerY + dot / 2 };
    POINT pt{ x, y };
    return PtInRect(&r, pt) != 0;
}

bool TaskTicketApp::HitTestMinimize(int x, int y) const
{
    int width = ScaleForDpi(Layout::WindowWidth);
    int headerH = ScaleForDpi(Layout::HeaderHeight);
    int dot = ScaleForDpi(Layout::DotDiameter);
    int rightMargin = ScaleForDpi(Layout::HeaderRightMargin);
    int centerY = headerH / 2;

    int yellowRight = width - rightMargin;
    int yellowLeft = yellowRight - dot;

    RECT r = { yellowLeft, centerY - dot / 2, yellowRight, centerY + dot / 2 };
    POINT pt{ x, y };
    return PtInRect(&r, pt) != 0;
}

bool TaskTicketApp::HitTestHeaderDraggable(int x, int y) const
{
    int headerH = ScaleForDpi(Layout::HeaderHeight);
    if (y < 0 || y >= headerH) return false;
    if (HitTestPin(x, y) || HitTestClose(x, y) || HitTestMinimize(x, y)) return false;
    return true;
}

int TaskTicketApp::HitTestTaskRow(int x, int y) const
{
    for (size_t i = 0; i < m_rowLayout.size(); ++i)
    {
        POINT pt{ x, y };
        if (PtInRect(&m_rowLayout[i].row, pt)) return (int)i;
    }
    return -1;
}

bool TaskTicketApp::HitTestCheckbox(int x, int y, int taskIndex) const
{
    if (taskIndex < 0 || taskIndex >= (int)m_rowLayout.size()) return false;
    POINT pt{ x, y };
    return PtInRect(&m_rowLayout[taskIndex].checkbox, pt) != 0;
}

bool TaskTicketApp::HitTestAddArea(int x, int y) const
{
    RECT client{};
    GetClientRect(m_hwnd, &client);

    int top = m_rowLayout.empty()
        ? ScaleForDpi(Layout::HeaderHeight + Layout::DividerThickness +
                      Layout::ClockAreaHeight + Layout::GapAfterClock)
        : m_rowLayout.back().row.bottom;

    RECT r = { 0, top, client.right, client.bottom };
    POINT pt{ x, y };
    return PtInRect(&r, pt) != 0;
}

//===========================================================================
// Mouse handling
//===========================================================================

void TaskTicketApp::OnLButtonDown(int x, int y)
{
    if (m_hwndEdit)
    {
        // Any click outside the active editor commits/cancels it first.
        RECT er{};
        GetWindowRect(m_hwndEdit, &er);
        POINT screen{ x, y };
        ClientToScreen(m_hwnd, &screen);
        if (!PtInRect(&er, screen))
        {
            OnEditCommit();
        }
    }

    if (HitTestClose(x, y) || HitTestMinimize(x, y) || HitTestPin(x, y))
    {
        // Handled on button-up (standard button feel); nothing to do here
        // besides suppressing drag-start below.
        return;
    }

    int taskIndex = HitTestTaskRow(x, y);
    if (taskIndex >= 0)
    {
        if (HitTestCheckbox(x, y, taskIndex))
        {
            ToggleTaskChecked(taskIndex);
        }
        else
        {
            m_selectedIndex = taskIndex;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }

    if (HitTestAddArea(x, y))
    {
        BeginAddTask();
        return;
    }

    if (!m_positionLocked && HitTestHeaderDraggable(x, y))
    {
        ReleaseCapture();
        SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
}

void TaskTicketApp::OnLButtonUp(int x, int y)
{
    if (HitTestClose(x, y))
    {
        DestroyWindow(m_hwnd);
        return;
    }
    if (HitTestMinimize(x, y))
    {
        ShowWindow(m_hwnd, SW_MINIMIZE);
        return;
    }
    if (HitTestPin(x, y))
    {
        TogglePinned();
        return;
    }
}

void TaskTicketApp::OnMouseMove(int x, int y)
{
    bool needRepaint = false;

    bool pin = HitTestPin(x, y);
    bool close = HitTestClose(x, y);
    bool min = HitTestMinimize(x, y);
    if (pin != m_hoverPin || close != m_hoverClose || min != m_hoverMinimize)
    {
        m_hoverPin = pin; m_hoverClose = close; m_hoverMinimize = min;
        needRepaint = true;
    }

    int taskIndex = HitTestTaskRow(x, y);
    int newHover = taskIndex >= 0 ? taskIndex : (HitTestAddArea(x, y) ? -2 : -1);
    if (newHover != m_hoverIndex)
    {
        m_hoverIndex = newHover;
        needRepaint = true;
    }

    if (needRepaint) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskTicketApp::OnMouseLeave()
{
    if (m_hoverIndex != -1 || m_hoverPin || m_hoverClose || m_hoverMinimize)
    {
        m_hoverIndex = -1;
        m_hoverPin = m_hoverClose = m_hoverMinimize = false;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void TaskTicketApp::OnRButtonUp(int x, int y)
{
    POINT screenPt{ x, y };
    ClientToScreen(m_hwnd, &screenPt);

    int taskIndex = HitTestTaskRow(x, y);
    if (taskIndex >= 0)
    {
        m_selectedIndex = taskIndex;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        ShowTaskContextMenu(taskIndex, screenPt);
        return;
    }

    int headerH = ScaleForDpi(Layout::HeaderHeight);
    if (y >= 0 && y < headerH && !HitTestPin(x, y) && !HitTestClose(x, y) && !HitTestMinimize(x, y))
    {
        ShowHeaderContextMenu(screenPt);
    }
}

void TaskTicketApp::OnKeyDown(WPARAM key)
{
    if (key == VK_DELETE && m_selectedIndex >= 0 && m_selectedIndex < (int)m_state.tasks.size())
    {
        DeleteTask(m_selectedIndex);
    }
}

//===========================================================================
// Context menus
//===========================================================================

void TaskTicketApp::ShowTaskContextMenu(int taskIndex, POINT screenPt)
{
    if (taskIndex < 0 || taskIndex >= (int)m_state.tasks.size()) return;

    HMENU menu = CreatePopupMenu();
    bool recurring = m_state.tasks[taskIndex].recurring;

    AppendMenuW(menu, MF_STRING, MENU_DELETE, L"Delete");
    AppendMenuW(menu, MF_STRING, MENU_TOGGLE_RECURRING,
                recurring ? L"Remove Recurring" : L"Mark as Recurring");

    SetForegroundWindow(m_hwnd); // ensures the menu closes properly on focus loss
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              screenPt.x, screenPt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == MENU_DELETE)
    {
        DeleteTask(taskIndex);
    }
    else if (cmd == MENU_TOGGLE_RECURRING)
    {
        ToggleTaskRecurring(taskIndex);
    }
}

void TaskTicketApp::ShowHeaderContextMenu(POINT screenPt)
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_TOGGLE_LOCK,
                m_positionLocked ? L"Unlock Position" : L"Lock Position");

    SetForegroundWindow(m_hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              screenPt.x, screenPt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == MENU_TOGGLE_LOCK)
    {
        TogglePositionLocked();
    }
}

//===========================================================================
// Toggles
//===========================================================================

void TaskTicketApp::TogglePinned()
{
    m_pinned = !m_pinned;
    ApplyTopmost();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskTicketApp::TogglePositionLocked()
{
    m_positionLocked = !m_positionLocked;
}

//===========================================================================
// Task operations
//===========================================================================

void TaskTicketApp::AddTask(const std::wstring& text)
{
    std::wstring trimmed = text;
    size_t start = trimmed.find_first_not_of(L" \t");
    size_t end = trimmed.find_last_not_of(L" \t");
    if (start == std::wstring::npos)
    {
        return; // whitespace-only - ignore
    }
    trimmed = trimmed.substr(start, end - start + 1);

    Task t;
    t.id = m_state.nextId++;
    t.text = trimmed;
    t.checked = false;
    t.recurring = false;
    m_state.tasks.push_back(t);

    RecalcLayout();
    ResizeWindowToContent();
    Save();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskTicketApp::DeleteTask(int index)
{
    if (index < 0 || index >= (int)m_state.tasks.size()) return;

    m_state.tasks.erase(m_state.tasks.begin() + index);
    if (m_selectedIndex == index) m_selectedIndex = -1;
    else if (m_selectedIndex > index) m_selectedIndex--;

    RecalcLayout();
    ResizeWindowToContent();
    Save();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskTicketApp::ToggleTaskChecked(int index)
{
    if (index < 0 || index >= (int)m_state.tasks.size()) return;
    m_state.tasks[index].checked = !m_state.tasks[index].checked;
    Save();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskTicketApp::ToggleTaskRecurring(int index)
{
    if (index < 0 || index >= (int)m_state.tasks.size()) return;
    m_state.tasks[index].recurring = !m_state.tasks[index].recurring;
    Save();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

//===========================================================================
// Inline "add task" editor
//===========================================================================

namespace
{
    WNDPROC g_originalEditProc = nullptr;

    LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* app = reinterpret_cast<TaskTicketApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (msg == WM_KEYDOWN)
        {
            if (wParam == VK_RETURN)
            {
                if (app) app->OnEditCommitPublic();
                return 0;
            }
            if (wParam == VK_ESCAPE)
            {
                if (app) app->OnEditCancelPublic();
                return 0;
            }
        }
        else if (msg == WM_KILLFOCUS)
        {
            if (app) app->OnEditCommitPublic();
            return 0;
        }

        return CallWindowProcW(g_originalEditProc, hwnd, msg, wParam, lParam);
    }
}

void TaskTicketApp::PositionAddTaskEdit()
{
    if (!m_hwndEdit) return;

    int top = m_rowLayout.empty()
        ? ScaleForDpi(Layout::HeaderHeight + Layout::DividerThickness +
                      Layout::ClockAreaHeight + Layout::GapAfterClock)
        : m_rowLayout.back().row.bottom;

    int rowH = ScaleForDpi(Layout::TaskRowHeight);
    int sideMargin = ScaleForDpi(Layout::SideMargin);
    int cbSize = ScaleForDpi(Layout::CheckboxSize);
    int width = ScaleForDpi(Layout::WindowWidth);

    int left = sideMargin + cbSize + ScaleForDpi(Layout::CheckboxTextGap);
    int editHeight = ScaleForDpi(24);
    int editTop = top + (rowH - editHeight) / 2;

    SetWindowPos(m_hwndEdit, nullptr, left, editTop,
                 width - left - sideMargin, editHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void TaskTicketApp::BeginAddTask()
{
    if (m_hwndEdit) return;

    m_hwndEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 10, 10,
        m_hwnd, (HMENU)(UINT_PTR)IDC_EDIT_ADDTASK, m_hInstance, nullptr);

    if (!m_hwndEdit) return;

    SendMessageW(m_hwndEdit, WM_SETFONT, (WPARAM)m_fontTask, TRUE);
    SetWindowLongPtrW(m_hwndEdit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    g_originalEditProc = (WNDPROC)SetWindowLongPtrW(
        m_hwndEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditSubclassProc));

    PositionAddTaskEdit();
    SetFocus(m_hwndEdit);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskTicketApp::OnEditCommit()
{
    if (!m_hwndEdit) return;

    wchar_t buf[512];
    GetWindowTextW(m_hwndEdit, buf, 512);
    std::wstring text = buf;

    HWND edit = m_hwndEdit;
    m_hwndEdit = nullptr; // clear before destroy so WM_KILLFOCUS re-entrancy is a no-op
    DestroyWindow(edit);

    if (!text.empty())
    {
        AddTask(text);
    }
    else
    {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void TaskTicketApp::OnEditCancel()
{
    if (!m_hwndEdit) return;
    HWND edit = m_hwndEdit;
    m_hwndEdit = nullptr;
    DestroyWindow(edit);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}
