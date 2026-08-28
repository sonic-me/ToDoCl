#include "TaskTicket.h"
#include "Resource.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <algorithm>
#include <cwchar>

#pragma comment(lib, "dwmapi.lib")

//===========================================================================
// Undocumented DWM "acrylic blur behind" API. Not exposed in any public
// header, so we declare the minimal pieces ourselves and resolve the
// function pointer dynamically (fails gracefully on systems that don't
// support it - see ApplyAcrylicBackground()).
//===========================================================================
namespace
{
    enum ACCENT_STATE
    {
        ACCENT_DISABLED = 0,
        ACCENT_ENABLE_GRADIENT = 1,
        ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
        ACCENT_ENABLE_BLURBEHIND = 3,
        ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
        ACCENT_ENABLE_HOSTBACKDROP = 5,
        ACCENT_INVALID_STATE = 6
    };

    struct ACCENT_POLICY
    {
        ACCENT_STATE AccentState;
        DWORD        AccentFlags;
        DWORD        GradientColor; // 0xAABBGGRR
        DWORD        AnimationId;
    };

    enum WINDOWCOMPOSITIONATTRIB
    {
        WCA_ACCENT_POLICY = 19
    };

    struct WINDOWCOMPOSITIONATTRIBDATA
    {
        WINDOWCOMPOSITIONATTRIB Attrib;
        PVOID                   pvData;
        SIZE_T                  cbData;
    };

    using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
}

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

//===========================================================================
// Colors & fonts
//===========================================================================
namespace Colors
{
    constexpr COLORREF Background   = RGB(14, 18, 24);
    constexpr COLORREF Divider      = RGB(46, 51, 59);
    constexpr COLORREF TitleText    = RGB(230, 231, 235);
    constexpr COLORREF TaskText     = RGB(231, 222, 201);
    constexpr COLORREF ClockText    = RGB(140, 144, 152);
    constexpr COLORREF CheckboxBorder = RGB(190, 190, 196);
    constexpr COLORREF CheckedFill  = RGB(96, 209, 197);
    constexpr COLORREF CheckMark    = RGB(14, 18, 24);
    constexpr COLORREF HoverRow     = RGB(58, 57, 53);
    constexpr COLORREF SelectedRow  = RGB(80, 77, 68);
    constexpr COLORREF PinBoxBg     = RGB(30, 36, 42);
    constexpr COLORREF PinCyan      = RGB(101, 214, 202);
    constexpr COLORREF PinMuted     = RGB(118, 132, 126);
    constexpr COLORREF CloseRed     = RGB(224, 76, 73);
    constexpr COLORREF MinimizeYel  = RGB(231, 180, 33);
    constexpr COLORREF AddHintText  = RGB(96, 100, 108);
}

//===========================================================================
// Construction / destruction
//===========================================================================

TaskTicketApp::TaskTicketApp(HINSTANCE hInstance)
    : m_hInstance(hInstance)
{
}

TaskTicketApp::~TaskTicketApp()
{
    ReleaseFonts();
}

int TaskTicketApp::ScaleForDpi(int value) const
{
    return MulDiv(value, (int)m_dpi, 96);
}

//===========================================================================
// Window creation
//===========================================================================

bool TaskTicketApp::Create()
{
    m_state = LoadState();
    PerformDailyResetIfNeeded(false);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = &TaskTicketApp::StaticWndProc;
    wc.hInstance     = m_hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // we paint everything ourselves
    wc.lpszClassName = L"TaskTicketWindowClass";
    wc.hIcon         = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APPICON));

    if (!RegisterClassExW(&wc))
    {
        // Icon resource may not exist in this build - retry without it.
        wc.hIcon = nullptr;
        if (!RegisterClassExW(&wc)) return false;
    }

    // Figure out a starting DPI from the primary monitor before we have an
    // HWND (needed to scale the initial window rectangle correctly).
    HDC screenDC = GetDC(nullptr);
    UINT startDpi = screenDC ? (UINT)GetDeviceCaps(screenDC, LOGPIXELSX) : 96;
    if (screenDC) ReleaseDC(nullptr, screenDC);
    m_dpi = startDpi ? startDpi : 96;

    int width  = MulDiv(Layout::WindowWidth, (int)m_dpi, 96);
    int height = MulDiv(Layout::MinWindowHeight, (int)m_dpi, 96);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (m_state.windowX != INT_MIN && m_state.windowY != INT_MIN)
    {
        x = m_state.windowX;
        y = m_state.windowY;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"Task Ticket - task.txt",
        WS_POPUP | WS_CLIPCHILDREN,
        x, y, width, height,
        nullptr, nullptr, m_hInstance, this);

    if (!m_hwnd) return false;

    // If we used CW_USEDEFAULT or the saved position, validate it now that
    // we have a real HWND (need the monitor it landed on).
    RECT wr{};
    GetWindowRect(m_hwnd, &wr);
    HMONITOR mon = MonitorFromRect(&wr, MONITOR_DEFAULTTONULL);
    if (mon == nullptr)
    {
        // Saved position is off-screen (monitor unplugged, etc.) - snap
        // back onto the primary monitor.
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int newX = work.left + 40;
        int newY = work.top + 40;
        SetWindowPos(m_hwnd, nullptr, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    ApplyRoundedCorners();
    ApplyAcrylicBackground();
    ApplyTopmost(); // default unpinned -> ensures normal Z-order

    RecalcLayout();
    ResizeWindowToContent();

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_hwnd);

    SetTimer(m_hwnd, ID_TIMER_CLOCK, 1000, nullptr);

    return true;
}

int TaskTicketApp::RunMessageLoop()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

//===========================================================================
// Window procedure plumbing
//===========================================================================

LRESULT CALLBACK TaskTicketApp::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    TaskTicketApp* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<TaskTicketApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<TaskTicketApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) return self->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TaskTicketApp::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        OnCreate(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1; // we always fully repaint in WM_PAINT - avoids flicker

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_SIZE:
        OnSize();
        return 0;

    case WM_DPICHANGED:
        OnDpiChanged(wParam, lParam);
        return 0;

    case WM_TIMER:
        if (wParam == ID_TIMER_CLOCK) OnTimer();
        return 0;

    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONUP:
        ReleaseCapture();
        OnLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    }

    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;

    case WM_RBUTTONUP:
        OnRButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;

    case WM_SETCURSOR:
    {
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            bool interactive = HitTestPin(pt.x, pt.y) || HitTestClose(pt.x, pt.y) ||
                                HitTestMinimize(pt.x, pt.y) ||
                                HitTestTaskRow(pt.x, pt.y) >= 0 ||
                                HitTestAddArea(pt.x, pt.y);
            SetCursor(LoadCursorW(nullptr, interactive ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }

    case WM_DESTROY:
        OnDestroy();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//===========================================================================
// Lifecycle
//===========================================================================

void TaskTicketApp::OnCreate(HWND hwnd)
{
    m_hwnd = hwnd;
    EnsureFonts();
}

void TaskTicketApp::OnDestroy()
{
    // Persist final window position.
    RECT wr{};
    if (GetWindowRect(m_hwnd, &wr))
    {
        m_state.windowX = wr.left;
        m_state.windowY = wr.top;
    }
    Save();
    KillTimer(m_hwnd, ID_TIMER_CLOCK);
}

void TaskTicketApp::EnsureFonts()
{
    ReleaseFonts();

    auto makeFont = [this](int pointSizePx, bool bold) -> HFONT
    {
        int heightPx = -ScaleForDpi(pointSizePx);
        HFONT f = CreateFontW(
            heightPx, 0, 0, 0,
            bold ? FW_SEMIBOLD : FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
            L"Cascadia Mono");

        if (!f)
        {
            f = CreateFontW(
                heightPx, 0, 0, 0,
                bold ? FW_SEMIBOLD : FW_NORMAL,
                FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                L"Consolas");
        }
        return f;
    };

    m_fontTitle = makeFont(15, false);
    m_fontTask  = makeFont(15, false);
    m_fontClock = makeFont(14, false);
}

void TaskTicketApp::ReleaseFonts()
{
    if (m_fontTitle) { DeleteObject(m_fontTitle); m_fontTitle = nullptr; }
    if (m_fontTask)  { DeleteObject(m_fontTask);  m_fontTask  = nullptr; }
    if (m_fontClock) { DeleteObject(m_fontClock); m_fontClock = nullptr; }
}

//===========================================================================
// DWM / composition effects
//===========================================================================

void TaskTicketApp::ApplyRoundedCorners()
{
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

    BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

void TaskTicketApp::ApplyAcrylicBackground()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setComposition = user32
        ? reinterpret_cast<SetWindowCompositionAttributeFn>(
              GetProcAddress(user32, "SetWindowCompositionAttribute"))
        : nullptr;

    m_compositionAvailable = false;

    if (setComposition)
    {
        // 0xAABBGGRR - dark matte navy tint, mostly opaque so the window
        // stays readable, with just enough alpha for the blur to show
        // through as a subtle acrylic effect.
        ACCENT_POLICY policy{};
        policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
        policy.AccentFlags = 0;
        policy.GradientColor = 0xE8181D24; // A=0xE8 (~91%), B=0x18,G=0x1D,R=0x24... see note
        policy.AnimationId = 0;

        WINDOWCOMPOSITIONATTRIBDATA data{};
        data.Attrib = WCA_ACCENT_POLICY;
        data.pvData = &policy;
        data.cbData = sizeof(policy);

        if (setComposition(m_hwnd, &data))
        {
            m_compositionAvailable = true;
        }
    }

    // If the API is unavailable (or failed), we simply fall back to the
    // fully opaque dark matte fill already used in Render() - no further
    // action needed here.
}

void TaskTicketApp::ApplyTopmost()
{
    SetWindowPos(m_hwnd, m_pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

//===========================================================================
// Layout
//===========================================================================

int TaskTicketApp::ContentHeight() const
{
    int header  = ScaleForDpi(Layout::HeaderHeight + Layout::DividerThickness);
    int clock   = ScaleForDpi(Layout::ClockAreaHeight + Layout::GapAfterClock);
    int rows    = ScaleForDpi(Layout::TaskRowHeight) * (int)m_state.tasks.size();
    int bottom  = ScaleForDpi(Layout::BottomPadding);
    return header + clock + rows + bottom;
}

void TaskTicketApp::RecalcLayout()
{
    m_rowLayout.clear();

    int width = ScaleForDpi(Layout::WindowWidth);
    int top = ScaleForDpi(Layout::HeaderHeight + Layout::DividerThickness +
                           Layout::ClockAreaHeight + Layout::GapAfterClock);
    int rowH = ScaleForDpi(Layout::TaskRowHeight);
    int sideMargin = ScaleForDpi(Layout::SideMargin);
    int cbSize = ScaleForDpi(Layout::CheckboxSize);

    for (size_t i = 0; i < m_state.tasks.size(); ++i)
    {
        TaskRowLayout rl{};
        rl.row = { 0, top, width, top + rowH };
        int cbTop = top + (rowH - cbSize) / 2;
        rl.checkbox = { sideMargin, cbTop, sideMargin + cbSize, cbTop + cbSize };
        m_rowLayout.push_back(rl);
        top += rowH;
    }
}

void TaskTicketApp::ResizeWindowToContent()
{
    int width = ScaleForDpi(Layout::WindowWidth);
    int height = ContentHeight();

    SetWindowPos(m_hwnd, nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void TaskTicketApp::OnSize()
{
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskTicketApp::OnDpiChanged(WPARAM wParam, LPARAM lParam)
{
    m_dpi = HIWORD(wParam);
    EnsureFonts();
    RecalcLayout();

    auto* suggested = reinterpret_cast<RECT*>(lParam);
    SetWindowPos(m_hwnd, nullptr,
                 suggested->left, suggested->top,
                 ScaleForDpi(Layout::WindowWidth), ContentHeight(),
                 SWP_NOZORDER | SWP_NOACTIVATE);

    if (m_hwndEdit) PositionAddTaskEdit();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

//===========================================================================
// Painting
//===========================================================================

void TaskTicketApp::OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);

    // Double buffer to avoid flicker - this is a single small bitmap, so
    // the CPU/memory cost is negligible and it only happens on repaint.
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    Render(memDC, client);

    BitBlt(hdc, 0, 0, client.right, client.bottom, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(hwnd, &ps);
}

namespace
{
    void FillRectColor(HDC hdc, const RECT& r, COLORREF color)
    {
        HBRUSH b = CreateSolidBrush(color);
        FillRect(hdc, &r, b);
        DeleteObject(b);
    }

    void DrawTextColored(HDC hdc, const std::wstring& text, RECT r, COLORREF color,
                          HFONT font, UINT format)
    {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, color);
        HFONT old = (HFONT)SelectObject(hdc, font);
        DrawTextW(hdc, text.c_str(), (int)text.size(), &r, format);
        SelectObject(hdc, old);
    }

    // Simple stylised pushpin glyph: round head + tapered needle.
    void DrawPinIcon(HDC hdc, const RECT& box, COLORREF color)
    {
        int w = box.right - box.left;
        int h = box.bottom - box.top;

        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);

        int headCx = box.left + (int)(w * 0.42);
        int headCy = box.top + (int)(h * 0.36);
        int headR = (int)(w * 0.16);

        Ellipse(hdc, headCx - headR, headCy - headR, headCx + headR, headCy + headR);

        POINT needle[3] = {
            { headCx - (int)(headR * 0.5), headCy + (int)(headR * 0.6) },
            { headCx + (int)(headR * 0.5), headCy + (int)(headR * 0.6) },
            { box.left + (int)(w * 0.74),  box.top  + (int)(h * 0.80) },
        };
        Polygon(hdc, needle, 3);

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }
}

void TaskTicketApp::Render(HDC hdc, const RECT& clientRect)
{
    // Base fill. When acrylic composition is active, DWM blends this with
    // a blurred copy of the desktop behind the window based on the accent
    // policy alpha, giving the "subtle acrylic" look; when unavailable we
    // simply get a solid dark matte window, which satisfies the graceful
    // fallback requirement.
    if (!m_compositionAvailable)
    {
        FillRectColor(hdc, clientRect, Colors::Background);
    }

    int width = clientRect.right;
    int headerH = ScaleForDpi(Layout::HeaderHeight);

    // ---- Header ----
    RECT titleRect = { ScaleForDpi(Layout::TitleLeftMargin), 0,
                        width - ScaleForDpi(220), headerH };
    DrawTextColored(hdc, L"Task Ticket - task.txt", titleRect, Colors::TitleText,
                     m_fontTitle, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);

    // Header controls: [ Pin box ] [ Red ] [ Yellow ], right aligned.
    int dot = ScaleForDpi(Layout::DotDiameter);
    int pinSize = ScaleForDpi(Layout::PinButtonSize);
    int spacing = ScaleForDpi(Layout::ButtonSpacing);
    int rightMargin = ScaleForDpi(Layout::HeaderRightMargin);
    int centerY = headerH / 2;

    int yellowRight = width - rightMargin;
    int yellowLeft = yellowRight - dot;
    RECT yellowRect = { yellowLeft, centerY - dot / 2, yellowRight, centerY + dot / 2 };

    int redRight = yellowLeft - spacing;
    int redLeft = redRight - dot;
    RECT redRect = { redLeft, centerY - dot / 2, redRight, centerY + dot / 2 };

    int pinRight = redLeft - spacing;
    int pinLeft = pinRight - pinSize;
    RECT pinRect = { pinLeft, centerY - pinSize / 2, pinRight, centerY + pinSize / 2 };

    // Pin box background
    {
        HBRUSH b = CreateSolidBrush(Colors::PinBoxBg);
        HPEN p = CreatePen(PS_SOLID, 1, Colors::PinBoxBg);
        HBRUSH ob = (HBRUSH)SelectObject(hdc, b);
        HPEN op = (HPEN)SelectObject(hdc, p);
        int r = ScaleForDpi(8);
        RoundRect(hdc, pinRect.left, pinRect.top, pinRect.right, pinRect.bottom, r, r);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(b);
        DeleteObject(p);
    }
    RECT pinIconRect = pinRect;
    InflateRect(&pinIconRect, -ScaleForDpi(7), -ScaleForDpi(7));
    DrawPinIcon(hdc, pinIconRect, m_pinned ? Colors::PinCyan : Colors::PinMuted);

    // Close (red) and minimize (yellow) dots
    {
        HBRUSH redBrush = CreateSolidBrush(Colors::CloseRed);
        HBRUSH oldB = (HBRUSH)SelectObject(hdc, redBrush);
        HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
        HPEN oldP = (HPEN)SelectObject(hdc, nullPen);
        Ellipse(hdc, redRect.left, redRect.top, redRect.right, redRect.bottom);
        SelectObject(hdc, oldB);
        DeleteObject(redBrush);

        HBRUSH yellowBrush = CreateSolidBrush(Colors::MinimizeYel);
        SelectObject(hdc, yellowBrush);
        Ellipse(hdc, yellowRect.left, yellowRect.top, yellowRect.right, yellowRect.bottom);
        SelectObject(hdc, oldP);
        DeleteObject(yellowBrush);
    }

    // ---- Divider ----
    RECT dividerRect = { 0, headerH, width, headerH + ScaleForDpi(Layout::DividerThickness) };
    FillRectColor(hdc, dividerRect, Colors::Divider);

    // ---- Clock ----
    wchar_t clockBuf[16];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    swprintf_s(clockBuf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

    int clockAreaTop = headerH + ScaleForDpi(Layout::DividerThickness);
    RECT clockRect = { 0, clockAreaTop, width - ScaleForDpi(Layout::SideMargin),
                        clockAreaTop + ScaleForDpi(Layout::ClockAreaHeight) };
    DrawTextColored(hdc, clockBuf, clockRect, Colors::ClockText, m_fontClock,
                     DT_SINGLELINE | DT_VCENTER | DT_RIGHT);

    // ---- Tasks ----
    for (size_t i = 0; i < m_state.tasks.size(); ++i)
    {
        const Task& task = m_state.tasks[i];
        const TaskRowLayout& rl = m_rowLayout[i];

        if ((int)i == m_selectedIndex)
        {
            FillRectColor(hdc, rl.row, Colors::SelectedRow);
        }
        else if ((int)i == m_hoverIndex)
        {
            FillRectColor(hdc, rl.row, Colors::HoverRow);
        }

        // Checkbox
        {
            HPEN pen = CreatePen(PS_SOLID, ScaleForDpi(1) > 1 ? ScaleForDpi(1) : 1,
                                  Colors::CheckboxBorder);
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            HBRUSH oldBrush;
            int r = ScaleForDpi(5);

            if (task.checked)
            {
                HBRUSH fill = CreateSolidBrush(Colors::CheckedFill);
                oldBrush = (HBRUSH)SelectObject(hdc, fill);
                RoundRect(hdc, rl.checkbox.left, rl.checkbox.top,
                          rl.checkbox.right, rl.checkbox.bottom, r, r);
                SelectObject(hdc, oldBrush);
                DeleteObject(fill);

                // Checkmark
                HPEN checkPen = CreatePen(PS_SOLID, std::max(2, ScaleForDpi(2)), Colors::CheckMark);
                SelectObject(hdc, checkPen);
                int cl = rl.checkbox.left, ct = rl.checkbox.top;
                int cw = rl.checkbox.right - rl.checkbox.left;
                int ch = rl.checkbox.bottom - rl.checkbox.top;
                POINT pts[3] = {
                    { cl + (int)(cw * 0.22), ct + (int)(ch * 0.52) },
                    { cl + (int)(cw * 0.42), ct + (int)(ch * 0.72) },
                    { cl + (int)(cw * 0.80), ct + (int)(ch * 0.28) },
                };
                Polyline(hdc, pts, 3);
                DeleteObject(checkPen);
            }
            else
            {
                oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
                RoundRect(hdc, rl.checkbox.left, rl.checkbox.top,
                          rl.checkbox.right, rl.checkbox.bottom, r, r);
                SelectObject(hdc, oldBrush);
            }

            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }

        // Label: "N. Task text"
        wchar_t numBuf[16];
        swprintf_s(numBuf, L"%d.", (int)(i + 1));
        std::wstring label = std::wstring(numBuf) + L" " + task.text;

        RECT textRect = rl.row;
        textRect.left = rl.checkbox.right + ScaleForDpi(Layout::CheckboxTextGap);
        textRect.right -= ScaleForDpi(Layout::SideMargin);

        DrawTextColored(hdc, label, textRect, Colors::TaskText, m_fontTask,
                         DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);

        if (task.recurring)
        {
            // Small "recurring" marker (circular arrow glyph substitute:
            // a filled dot) after the text, keeping things minimal.
            int d = ScaleForDpi(6);
            int cy = (rl.row.top + rl.row.bottom) / 2;
            int cx = textRect.right - d;
            RECT dotR = { cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2 };
            FillRectColor(hdc, dotR, Colors::PinCyan);
        }
    }

    // ---- Empty "add task" area hint ----
    if (m_hoverIndex == -2 && m_hwndEdit == nullptr)
    {
        RECT addRect{};
        if (!m_rowLayout.empty())
        {
            addRect = { 0, m_rowLayout.back().row.bottom, width, clientRect.bottom };
        }
        else
        {
            int top = ScaleForDpi(Layout::HeaderHeight + Layout::DividerThickness +
                                   Layout::ClockAreaHeight + Layout::GapAfterClock);
            addRect = { 0, top, width, clientRect.bottom };
        }
        RECT hintRect = addRect;
        hintRect.left += ScaleForDpi(Layout::SideMargin);
        hintRect.right -= ScaleForDpi(Layout::SideMargin);
        DrawTextColored(hdc, L"+ Add task", hintRect, Colors::AddHintText, m_fontTask,
                         DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
}

//===========================================================================
// Timer / daily reset
//===========================================================================

void TaskTicketApp::OnTimer()
{
    PerformDailyResetIfNeeded(false);

    // Only invalidate the small clock region each second, unless a full
    // reset just happened (PerformDailyResetIfNeeded invalidates itself
    // when it changes anything).
    int width = ScaleForDpi(Layout::WindowWidth);
    int headerH = ScaleForDpi(Layout::HeaderHeight + Layout::DividerThickness);
    RECT clockRect = { 0, headerH, width, headerH + ScaleForDpi(Layout::ClockAreaHeight) };
    InvalidateRect(m_hwnd, &clockRect, FALSE);
}

void TaskTicketApp::PerformDailyResetIfNeeded(bool forceInvalidateAll)
{
    std::wstring today = GetTodayDateString();
    if (m_state.lastDate == today && !forceInvalidateAll) return;

    bool anyChanged = false;
    if (m_state.lastDate != today)
    {
        for (Task& t : m_state.tasks)
        {
            if (t.recurring && t.checked)
            {
                t.checked = false;
                anyChanged = true;
            }
        }
        m_state.lastDate = today;
        anyChanged = true; // date itself changed - always worth saving
    }

    if (anyChanged)
    {
        Save();
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

//===========================================================================
// Persistence glue
//===========================================================================

void TaskTicketApp::Save()
{
    SaveState(m_state);
}
