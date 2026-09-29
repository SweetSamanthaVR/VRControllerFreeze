#include "main_window.h"

#include <objidl.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

// GDI+ expects the min/max macros that NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include "resource.h"

namespace vr_controller_freeze::app {
namespace {

constexpr wchar_t kWindowClass[] = L"VRControllerFreezeWindowClass";
constexpr wchar_t kHeading[] = L"VR Controller Freeze";
constexpr wchar_t kSubheading[] = L"Holds a controller's position while its buttons, sticks and fingers stay live.";
constexpr UINT_PTR kTickTimer = 1;
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
constexpr size_t kMaxLogChars = 61000; // slightly above AppLog's bound, so the edit never truncates it

// Layout, in 96-DPI units.
constexpr int kMargin = 20;
constexpr int kGap = 12;
constexpr int kCardPadding = 16;
constexpr int kButtonHeight = 36;
constexpr int kToolbarHeight = 32;
constexpr int kPillHeight = 30;
constexpr int kInitialClientWidth = 880;
constexpr int kInitialClientHeight = 780;
constexpr int kMinClientWidth = 720;
constexpr int kMinClientHeight = 660;

enum ControlId { IDC_LEFT = 1001, IDC_RIGHT, IDC_BOTH, IDC_BINDINGS, IDC_RELEASE, IDC_LOG, IDC_DIAGNOSTICS, IDC_OPEN_LOG, IDC_CLEAR_LOG, IDC_THEME };

struct Palette {
    COLORREF background;
    COLORREF surface;         // cards and status pills
    COLORREF surfaceBorder;
    COLORREF frozenSurface;   // a card whose hand is frozen
    COLORREF text;
    COLORREF muted;
    COLORREF accent;
    COLORREF onAccent;
    COLORREF good;
    COLORREF warning;
    COLORREF error;
    COLORREF secondary;       // secondary button fill
    COLORREF secondaryBorder;
    COLORREF danger;
    COLORREF onDanger;
    COLORREF logBackground;
    COLORREF logText;
    COLORREF logBorder;
};

constexpr Palette kDark{
    RGB(30, 31, 34),   RGB(43, 45, 49),   RGB(58, 60, 65),   RGB(26, 46, 60),   RGB(242, 242, 242), RGB(160, 164, 170),
    RGB(76, 194, 255), RGB(0, 31, 46),    RGB(108, 203, 95), RGB(255, 185, 0),  RGB(255, 107, 107),
    RGB(50, 52, 57),   RGB(72, 74, 80),   RGB(196, 43, 28),  RGB(255, 255, 255), RGB(22, 23, 25),   RGB(222, 222, 222),
    RGB(58, 60, 65)};

constexpr Palette kLight{
    RGB(243, 243, 243), RGB(255, 255, 255), RGB(222, 222, 222), RGB(232, 242, 251), RGB(27, 27, 27),   RGB(96, 99, 104),
    RGB(0, 95, 184),    RGB(255, 255, 255), RGB(15, 123, 15),   RGB(157, 93, 0),    RGB(196, 43, 28),
    RGB(255, 255, 255), RGB(204, 204, 204), RGB(196, 43, 28),   RGB(255, 255, 255), RGB(255, 255, 255), RGB(27, 27, 27),
    RGB(209, 209, 209)};

COLORREF Mix(COLORREF from, COLORREF to, double amount) {
    auto channel = [&](int a, int b) { return static_cast<BYTE>(std::lround(a + (b - a) * amount)); };
    return RGB(channel(GetRValue(from), GetRValue(to)), channel(GetGValue(from), GetGValue(to)), channel(GetBValue(from), GetBValue(to)));
}

Gdiplus::Color ToGdiplus(COLORREF colour) { return Gdiplus::Color(255, GetRValue(colour), GetGValue(colour), GetBValue(colour)); }

// Fills a rounded rectangle and optionally outlines it, anti-aliased and aligned to whole pixels.
void RoundedRect(Gdiplus::Graphics& graphics, const RECT& rect, float radius, COLORREF fill, COLORREF border, float borderWidth) {
    const float inset = borderWidth > 0 ? borderWidth / 2.0f : 0.5f;
    const float x = static_cast<float>(rect.left) + inset;
    const float y = static_cast<float>(rect.top) + inset;
    const float width = static_cast<float>(rect.right - rect.left) - 2 * inset;
    const float height = static_cast<float>(rect.bottom - rect.top) - 2 * inset;
    const float diameter = std::min(radius * 2, std::min(width, height));
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(ToGdiplus(fill));
    graphics.FillPath(&brush, &path);
    if (borderWidth > 0) {
        Gdiplus::Pen pen(ToGdiplus(border), borderWidth);
        graphics.DrawPath(&pen, &path);
    }
}

void Dot(Gdiplus::Graphics& graphics, int centreX, int centreY, int diameter, COLORREF colour) {
    Gdiplus::SolidBrush brush(ToGdiplus(colour));
    graphics.FillEllipse(&brush, static_cast<float>(centreX) - diameter / 2.0f, static_cast<float>(centreY) - diameter / 2.0f,
                         static_cast<float>(diameter), static_cast<float>(diameter));
}

// Single-line text is vertically centred and ends in an ellipsis if too long; DT_WORDBREAK text wraps.
void Text(HDC dc, HFONT font, COLORREF colour, const std::wstring& text, RECT rect, UINT format = DT_LEFT) {
    const HGDIOBJ previous = ::SelectObject(dc, font);
    ::SetTextColor(dc, colour);
    const UINT layout = (format & DT_WORDBREAK) ? DT_EDITCONTROL : (DT_SINGLELINE | DT_VCENTER);
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | layout | DT_NOPREFIX | DT_END_ELLIPSIS);
    ::SelectObject(dc, previous);
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
    const HGDIOBJ previous = ::SelectObject(dc, font);
    SIZE size{};
    ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    ::SelectObject(dc, previous);
    return size.cx;
}

int FontHeight(HWND hwnd, HFONT font) {
    HDC dc = ::GetDC(hwnd);
    const HGDIOBJ previous = ::SelectObject(dc, font);
    TEXTMETRICW metrics{};
    ::GetTextMetricsW(dc, &metrics);
    ::SelectObject(dc, previous);
    ::ReleaseDC(hwnd, dc);
    return metrics.tmHeight + metrics.tmExternalLeading;
}

// A multiline edit asks the dialog manager for every key, including Tab, and to have all its text
// selected when it gains focus. The log is read-only, so let Tab move on and skip the select-all;
// arrow keys and Page Up/Down still scroll it.
LRESULT CALLBACK LogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_GETDLGCODE) {
        return ::DefSubclassProc(hwnd, message, wParam, lParam) & ~(DLGC_WANTTAB | DLGC_WANTALLKEYS | DLGC_HASSETSEL);
    }
    if (message == WM_NCDESTROY) ::RemoveWindowSubclass(hwnd, LogProc, id);
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

} // namespace

MainWindow::~MainWindow() {
    for (HGDIOBJ object : {static_cast<HGDIOBJ>(uiFont_), static_cast<HGDIOBJ>(boldFont_), static_cast<HGDIOBJ>(titleFont_),
                           static_cast<HGDIOBJ>(stateFont_), static_cast<HGDIOBJ>(captionFont_), static_cast<HGDIOBJ>(logFont_),
                           static_cast<HGDIOBJ>(logBrush_)}) {
        if (object) ::DeleteObject(object);
    }
    if (gdiplusToken_) Gdiplus::GdiplusShutdown(gdiplusToken_);
}

bool MainWindow::Create(HINSTANCE instance, const std::wstring& title, bool darkMode, Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
    dark_ = darkMode;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusInput, nullptr) != Gdiplus::Ok) return false;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                        ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), 0));
    windowClass.hIconSm = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                          ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0));
    windowClass.hbrBackground = nullptr; // everything is painted in WM_PAINT
    windowClass.lpszClassName = kWindowClass;
    if (!::RegisterClassExW(&windowClass)) return false;

    hwnd_ = ::CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, title.c_str(), kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                              CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    // Size the window for its monitor's DPI now that it exists on one, keeping it on screen.
    ApplyDpi(::GetDpiForWindow(hwnd_));
    ApplyTheme();
    SIZE size = WindowSizeForClient(kInitialClientWidth, kInitialClientHeight);
    RECT placed{};
    ::GetWindowRect(hwnd_, &placed);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const RECT& work = monitor.rcWork;
        size.cx = std::min<LONG>(size.cx, work.right - work.left);
        size.cy = std::min<LONG>(size.cy, work.bottom - work.top);
        placed.left = std::clamp<LONG>(placed.left, work.left, work.right - size.cx);
        placed.top = std::clamp<LONG>(placed.top, work.top, work.bottom - size.cy);
    }
    ::SetWindowPos(hwnd_, nullptr, placed.left, placed.top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetTimer(hwnd_, kTickTimer, kTickMs, nullptr);
    return true;
}

void MainWindow::Show(int showCommand) {
    ::ShowWindow(hwnd_, showCommand);
    ::UpdateWindow(hwnd_);
}

int MainWindow::RunMessageLoop() {
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Gives the controls standard keyboard navigation (Tab, Shift+Tab, Space).
        if (::IsDialogMessageW(hwnd_, &message)) continue;
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void MainWindow::SetView(const WindowView& view) {
    if (view == view_) return;
    view_ = view;
    UpdateCardButtons();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetDarkMode(bool dark) {
    if (dark == dark_) return;
    dark_ = dark;
    ApplyTheme();
}

// New lines are followed only while the log is scrolled to the bottom. Once the reader scrolls up,
// their place and selection are kept, allowing for any lines AppLog trimmed from the top.
void MainWindow::SetLog(const std::wstring& text, size_t trimmedChars) {
    if (!log_) return;
    SCROLLINFO scroll{sizeof(scroll), SIF_POS | SIF_PAGE | SIF_RANGE};
    const bool atBottom = !::GetScrollInfo(log_, SB_VERT, &scroll) || scroll.nPage == 0 ||
                          scroll.nPos + static_cast<int>(scroll.nPage) > scroll.nMax;
    const size_t trimmed = atBottom ? std::wstring::npos : trimmedChars;
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    ::SendMessageW(log_, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
    const auto firstVisible = static_cast<size_t>(
        ::SendMessageW(log_, EM_LINEINDEX, ::SendMessageW(log_, EM_GETFIRSTVISIBLELINE, 0, 0), 0));

    ::SendMessageW(log_, WM_SETREDRAW, FALSE, 0);
    ::SetWindowTextW(log_, text.c_str());
    if (trimmed == std::wstring::npos) {
        // EM_SCROLLCARET does nothing while redrawing is off, so scroll by lines instead.
        ::SendMessageW(log_, EM_SETSEL, text.size(), text.size());
        ::SendMessageW(log_, EM_LINESCROLL, 0, ::SendMessageW(log_, EM_GETLINECOUNT, 0, 0));
    } else {
        auto shift = [trimmed](size_t position) -> WPARAM { return position > trimmed ? position - trimmed : 0; };
        ::SendMessageW(log_, EM_SETSEL, shift(selectionStart), static_cast<LPARAM>(shift(selectionEnd)));
        ::SendMessageW(log_, EM_LINESCROLL, 0, ::SendMessageW(log_, EM_LINEFROMCHAR, shift(firstVisible), 0));
    }
    ::SendMessageW(log_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(log_, nullptr, TRUE);
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(message, wParam, lParam) : ::DefWindowProcW(hwnd, message, wParam, lParam);
}

// Hover tracking for the drawn buttons, and a quick second click counted as a click rather than a
// double-click, which owner-drawn buttons would otherwise report instead.
LRESULT CALLBACK MainWindow::ButtonProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR self) {
    auto* window = reinterpret_cast<MainWindow*>(self);
    switch (message) {
    case WM_MOUSEMOVE: {
        ButtonInfo& info = window->buttons_[hwnd];
        if (!info.hovered) {
            info.hovered = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            ::TrackMouseEvent(&track);
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        window->buttons_[hwnd].hovered = false;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_LBUTTONDBLCLK:
        return ::DefSubclassProc(hwnd, WM_LBUTTONDOWN, wParam, lParam);
    case WM_ERASEBKGND:
        return 1; // the whole button is drawn in WM_DRAWITEM
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(hwnd, ButtonProc, id);
        break;
    default:
        break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateControls();
        return 0;
    case WM_TIMER:
        if (wParam == kTickTimer && callbacks_.onTick) callbacks_.onTick();
        return 0;
    case WM_COMMAND: {
        if (HIWORD(wParam) != BN_CLICKED) return 0;
        static constexpr struct { int id; Command command; } kButtons[] = {
            {IDC_LEFT, Command::ToggleLeft},         {IDC_RIGHT, Command::ToggleRight},
            {IDC_BOTH, Command::ToggleBoth},         {IDC_RELEASE, Command::EmergencyRelease},
            {IDC_BINDINGS, Command::Bindings},       {IDC_DIAGNOSTICS, Command::Diagnostics},
            {IDC_OPEN_LOG, Command::OpenLogFolder},  {IDC_CLEAR_LOG, Command::ClearLog},
            {IDC_THEME, Command::ToggleTheme},
        };
        for (const auto& button : kButtons) {
            if (LOWORD(wParam) == button.id && callbacks_.onCommand) callbacks_.onCommand(button.command);
        }
        return 0;
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType != ODT_BUTTON) return FALSE;
        DrawButton(*item);
        return TRUE;
    }
    case WM_ACTIVATE:
        // Remember the focused control when switching away, so switching back restores it.
        if (LOWORD(wParam) == WA_INACTIVE) {
            HWND focus = ::GetFocus();
            if (focus && ::IsChild(hwnd_, focus)) lastFocus_ = focus;
        }
        return ::DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_SETFOCUS:
        // Keyboard focus always belongs to a control: the last one used, or at first the Live Log,
        // so a stray key press cannot click a freeze button.
        ::SetFocus(lastFocus_ && ::IsWindow(lastFocus_) ? lastFocus_ : log_);
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        const SIZE minimum = WindowSizeForClient(kMinClientWidth, kMinClientHeight);
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {minimum.cx, minimum.cy};
        return 0;
    }
    case WM_DPICHANGED: {
        ApplyDpi(HIWORD(wParam));
        const RECT* suggested = reinterpret_cast<RECT*>(lParam);
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // WM_PAINT paints every pixel, off screen first, so there is no flicker
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(hwnd_, &paint);
        RECT client{};
        ::GetClientRect(hwnd_, &client);
        HDC buffer = ::CreateCompatibleDC(dc);
        HBITMAP bitmap = ::CreateCompatibleBitmap(dc, client.right, client.bottom);
        const HGDIOBJ previous = ::SelectObject(buffer, bitmap);
        Paint(buffer);
        ::BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        ::SelectObject(buffer, previous);
        ::DeleteObject(bitmap);
        ::DeleteDC(buffer);
        ::EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        // The log (a read-only edit reports as static) uses the theme's colours.
        const Palette& palette = dark_ ? kDark : kLight;
        auto dc = reinterpret_cast<HDC>(wParam);
        ::SetTextColor(dc, palette.logText);
        ::SetBkColor(dc, palette.logBackground);
        return reinterpret_cast<LRESULT>(logBrush_);
    }
    case WM_DESTROY:
        ::KillTimer(hwnd_, kTickTimer);
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd_, message, wParam, lParam);
    }
}

HWND MainWindow::CreateButton(const wchar_t* text, int id, ButtonStyle style, Surface surface) {
    HWND button = ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    buttons_[button] = {style, surface, false};
    ::SetWindowSubclass(button, ButtonProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return button;
}

// Creation order is the Tab order: the hands, the shared actions, the log, then the toolbar.
void MainWindow::CreateControls() {
    leftButton_ = CreateButton(L"Freeze Left", IDC_LEFT, ButtonStyle::Primary, Surface::LeftCard);
    rightButton_ = CreateButton(L"Freeze Right", IDC_RIGHT, ButtonStyle::Primary, Surface::RightCard);
    bothButton_ = CreateButton(L"Freeze Both", IDC_BOTH, ButtonStyle::Secondary, Surface::Window);
    bindingsButton_ = CreateButton(L"SteamVR Bindings", IDC_BINDINGS, ButtonStyle::Secondary, Surface::Window);
    releaseButton_ = CreateButton(L"Emergency Release", IDC_RELEASE, ButtonStyle::Danger, Surface::Window);
    log_ = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                             0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)), nullptr, nullptr);
    ::SendMessageW(log_, EM_SETLIMITTEXT, kMaxLogChars, 0);
    ::SetWindowSubclass(log_, LogProc, 1, 0);
    diagnosticsButton_ = CreateButton(L"Diagnostics", IDC_DIAGNOSTICS, ButtonStyle::Secondary, Surface::Window);
    openLogButton_ = CreateButton(L"Open Log Folder", IDC_OPEN_LOG, ButtonStyle::Secondary, Surface::Window);
    clearLogButton_ = CreateButton(L"Clear Log", IDC_CLEAR_LOG, ButtonStyle::Secondary, Surface::Window);
    themeButton_ = CreateButton(L"Light Mode", IDC_THEME, ButtonStyle::Secondary, Surface::Window);
}

// Recreates the fonts for the given DPI, all from the system message font (Segoe UI), with
// Consolas at the same size for the log.
void MainWindow::ApplyDpi(UINT dpi) {
    dpi_ = dpi ? dpi : 96;
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_);
    const LOGFONTW base = metrics.lfMessageFont;
    auto make = [&](double scale, LONG weight, const wchar_t* face) {
        LOGFONTW font = base;
        font.lfHeight = static_cast<LONG>(std::lround(base.lfHeight * scale));
        font.lfWeight = weight;
        if (face) {
            wcscpy_s(font.lfFaceName, face);
            font.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        }
        return ::CreateFontIndirectW(&font);
    };

    const HFONT old[] = {uiFont_, boldFont_, titleFont_, stateFont_, captionFont_, logFont_};
    uiFont_ = make(1.0, FW_NORMAL, nullptr);
    boldFont_ = make(1.0, FW_SEMIBOLD, nullptr);
    titleFont_ = make(1.6, FW_SEMIBOLD, nullptr);
    stateFont_ = make(1.7, FW_SEMIBOLD, nullptr);
    captionFont_ = make(0.85, FW_SEMIBOLD, nullptr);
    logFont_ = make(1.0, FW_NORMAL, L"Consolas");
    ::SendMessageW(log_, WM_SETFONT, reinterpret_cast<WPARAM>(logFont_), TRUE);
    for (HFONT font : old) {
        if (font) ::DeleteObject(font);
    }

    lineHeight_ = FontHeight(hwnd_, uiFont_);
    titleHeight_ = FontHeight(hwnd_, titleFont_);
    stateHeight_ = FontHeight(hwnd_, stateFont_);
    captionHeight_ = FontHeight(hwnd_, captionFont_);
    Layout();
}

// Applies the current theme to the title bar, the log and the drawn parts, then repaints.
void MainWindow::ApplyTheme() {
    const Palette& palette = dark_ ? kDark : kLight;
    const BOOL darkTitleBar = dark_ ? TRUE : FALSE; // Windows 10 20H1 and later; ignored elsewhere
    ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkTitleBar, sizeof(darkTitleBar));
    // The log's scroll bar uses the system's dark control style in dark mode; if that style is
    // unavailable it stays light but still works.
    ::SetWindowTheme(log_, dark_ ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (logBrush_) ::DeleteObject(logBrush_);
    logBrush_ = ::CreateSolidBrush(palette.logBackground);
    ::SetWindowTextW(themeButton_, dark_ ? L"Light Mode" : L"Dark Mode");
    ::RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void MainWindow::Layout() {
    if (!log_) return;
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int margin = Scale(kMargin);
    const int gap = Scale(kGap);
    const int padding = Scale(kCardPadding);
    const int buttonHeight = Scale(kButtonHeight);
    const int right = static_cast<int>(client.right) - margin;
    const int width = std::max(0, right - margin);

    // Header, then the status pills.
    int y = margin + titleHeight_ + Scale(2) + lineHeight_;
    pillsTop_ = y + Scale(16);
    y = pillsTop_ + Scale(kPillHeight) + Scale(18);

    // The hand cards, each with its own button along the bottom.
    const int cardWidth = std::max(0, (width - gap) / 2);
    const int cardHeight = padding + captionHeight_ + Scale(4) + stateHeight_ + Scale(8) + lineHeight_ + Scale(4) +
                           lineHeight_ + Scale(6) + 2 * lineHeight_ + Scale(14) + buttonHeight + padding;
    leftCard_ = {margin, y, margin + cardWidth, y + cardHeight};
    rightCard_ = {right - cardWidth, y, right, y + cardHeight};
    for (const auto& [card, button] : {std::pair{leftCard_, leftButton_}, std::pair{rightCard_, rightButton_}}) {
        ::MoveWindow(button, card.left + padding, card.bottom - padding - buttonHeight, card.right - card.left - 2 * padding,
                     buttonHeight, TRUE);
    }
    y += cardHeight + Scale(14);

    // Shared actions: Emergency Release stands apart on the right.
    ::MoveWindow(bothButton_, margin, y, Scale(160), buttonHeight, TRUE);
    ::MoveWindow(bindingsButton_, margin + Scale(160) + gap, y, Scale(170), buttonHeight, TRUE);
    ::MoveWindow(releaseButton_, right - Scale(180), y, Scale(180), buttonHeight, TRUE);
    y += buttonHeight + Scale(22);

    // The Live Log fills the space down to the toolbar; the window draws its border.
    logLabelTop_ = y;
    y += lineHeight_ + Scale(8);
    const int toolbarHeight = Scale(kToolbarHeight);
    const int toolbarTop = static_cast<int>(client.bottom) - margin - toolbarHeight;
    ::MoveWindow(log_, margin + 1, y + 1, std::max(0, width - 2), std::max(0, toolbarTop - Scale(12) - y - 2), TRUE);
    const int logPadding = Scale(6);
    ::SendMessageW(log_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(logPadding, logPadding));

    const int toolWidth = Scale(140);
    const HWND tools[] = {diagnosticsButton_, openLogButton_, clearLogButton_};
    for (int i = 0; i < static_cast<int>(std::size(tools)); ++i) {
        ::MoveWindow(tools[i], margin + i * (toolWidth + Scale(8)), toolbarTop, toolWidth, toolbarHeight, TRUE);
    }
    ::MoveWindow(themeButton_, right - toolWidth, toolbarTop, toolWidth, toolbarHeight, TRUE);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

// A hand's button freezes (the primary action) or, while the hand is held, unfreezes. When freezing
// cannot work it is shown quiet but stays clickable, so a click still logs the reason.
void MainWindow::UpdateCardButtons() {
    auto update = [&](HWND button, bool held, bool canFreeze, const wchar_t* freeze, const wchar_t* unfreeze) {
        const wchar_t* text = held ? unfreeze : freeze;
        wchar_t current[64]{};
        ::GetWindowTextW(button, current, static_cast<int>(std::size(current)));
        if (std::wcscmp(current, text) != 0) ::SetWindowTextW(button, text);
        buttons_[button].style = held ? ButtonStyle::Secondary : (canFreeze ? ButtonStyle::Primary : ButtonStyle::Quiet);
        ::InvalidateRect(button, nullptr, FALSE);
    };
    update(leftButton_, view_.left.held, view_.left.canFreeze, L"Freeze Left", L"Unfreeze Left");
    update(rightButton_, view_.right.held, view_.right.canFreeze, L"Freeze Right", L"Unfreeze Right");
    ::SetWindowTextW(bothButton_, (view_.left.held && view_.right.held) ? L"Unfreeze Both" : L"Freeze Both");
}

void MainWindow::Paint(HDC dc) {
    const Palette& palette = dark_ ? kDark : kLight;
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    HBRUSH background = ::CreateSolidBrush(palette.background);
    ::FillRect(dc, &client, background);
    ::DeleteObject(background);
    ::SetBkMode(dc, TRANSPARENT);
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    const int margin = Scale(kMargin);
    const int right = static_cast<int>(client.right) - margin;

    // Header.
    Text(dc, titleFont_, palette.text, kHeading, {margin, margin, right, margin + titleHeight_});
    const int subtitleTop = margin + titleHeight_ + Scale(2);
    Text(dc, uiFont_, palette.muted, kSubheading, {margin, subtitleTop, right, subtitleTop + lineHeight_});

    // Status pills: a coloured dot, a muted label and the value.
    int x = margin;
    const int pillHeight = Scale(kPillHeight);
    const int pillPadding = Scale(12);
    const int dot = Scale(8);
    for (const auto& [label, summary] : {std::pair{L"SteamVR:", view_.steamVr}, std::pair{L"Freeze driver:", view_.driver},
                                         std::pair{L"Pose hook:", view_.hook}}) {
        const int labelWidth = TextWidth(dc, uiFont_, label);
        const int valueWidth = TextWidth(dc, boldFont_, summary.text);
        if (x >= right) break;
        // A pill that would run past the edge is shortened; its value then ends in an ellipsis.
        const int pillWidth = std::min(pillPadding + dot + Scale(8) + labelWidth + Scale(6) + valueWidth + pillPadding + Scale(2), right - x);
        const RECT pill{x, pillsTop_, x + pillWidth, pillsTop_ + pillHeight};
        RoundedRect(graphics, pill, pillHeight / 2.0f, palette.surface, palette.surfaceBorder, 1.0f);
        Dot(graphics, x + pillPadding + dot / 2, pillsTop_ + pillHeight / 2, dot, ToneColour(summary.tone));
        int textLeft = x + pillPadding + dot + Scale(8);
        Text(dc, uiFont_, palette.muted, label, {textLeft, pill.top, textLeft + labelWidth, pill.bottom});
        textLeft += labelWidth + Scale(6);
        const bool alarming = summary.tone == Tone::Warning || summary.tone == Tone::Error;
        Text(dc, boldFont_, alarming ? ToneColour(summary.tone) : palette.text, summary.text,
             {textLeft, pill.top, pill.right - pillPadding, pill.bottom});
        x += pillWidth + Scale(8);
    }

    PaintHandCard(dc, leftCard_, L"LEFT HAND", view_.left, Surface::LeftCard);
    PaintHandCard(dc, rightCard_, L"RIGHT HAND", view_.right, Surface::RightCard);

    // The Live Log heading and a one-pixel border around the log.
    Text(dc, boldFont_, palette.text, L"Live Log", {margin, logLabelTop_, right, logLabelTop_ + lineHeight_});
    RECT frame{};
    ::GetWindowRect(log_, &frame);
    ::MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&frame), 2);
    ::InflateRect(&frame, 1, 1);
    HBRUSH border = ::CreateSolidBrush(palette.logBorder);
    ::FrameRect(dc, &frame, border);
    ::DeleteObject(border);
}

// A card: the hand, its state in large type, the controller, whether its movement is coming
// through, and a note. A frozen hand's card is outlined in the state's colour, and tinted while
// the freeze is in effect.
void MainWindow::PaintHandCard(HDC dc, const RECT& card, const wchar_t* title, const HandPanelView& hand, Surface surface) {
    const Palette& palette = dark_ ? kDark : kLight;
    const Tone stateTone = hand.summary.state.tone;
    const bool frozen = stateTone == Tone::Accent || stateTone == Tone::Warning;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    RoundedRect(graphics, card, static_cast<float>(Scale(10)), SurfaceColour(surface), frozen ? ToneColour(stateTone) : palette.surfaceBorder,
                frozen ? static_cast<float>(Scale(2)) : 1.0f);

    const int padding = Scale(kCardPadding);
    const int left = card.left + padding;
    const int right = card.right - padding;
    int y = card.top + padding;
    Text(dc, captionFont_, palette.muted, title, {left, y, right, y + captionHeight_});
    y += captionHeight_ + Scale(4);
    Text(dc, stateFont_, ToneColour(stateTone), hand.summary.state.text, {left, y, right, y + stateHeight_});
    y += stateHeight_ + Scale(8);
    Text(dc, uiFont_, palette.text, hand.device, {left, y, right, y + lineHeight_});
    y += lineHeight_ + Scale(4);
    const int dot = Scale(8);
    Dot(graphics, left + dot / 2, y + lineHeight_ / 2, dot, ToneColour(hand.tracking.tone));
    const COLORREF trackingColour = hand.tracking.tone == Tone::Good ? palette.text : ToneColour(hand.tracking.tone);
    Text(dc, uiFont_, trackingColour, hand.tracking.text, {left + dot + Scale(8), y, right, y + lineHeight_});
    y += lineHeight_ + Scale(6);
    Text(dc, uiFont_, ToneColour(hand.summary.note.tone), hand.summary.note.text, {left, y, right, y + 2 * lineHeight_},
         DT_LEFT | DT_WORDBREAK);
}

// Draws a button: filled for the primary and danger actions, outlined for the rest, with hover,
// pressed and keyboard-focus states.
void MainWindow::DrawButton(const DRAWITEMSTRUCT& item) {
    const Palette& palette = dark_ ? kDark : kLight;
    const auto found = buttons_.find(item.hwndItem);
    const ButtonInfo info = found != buttons_.end() ? found->second : ButtonInfo{};
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0;

    COLORREF fill = palette.secondary;
    COLORREF border = palette.secondaryBorder;
    COLORREF textColour = info.style == ButtonStyle::Quiet ? palette.muted : palette.text;
    float borderWidth = 1.0f;
    HFONT font = uiFont_;
    if (info.style == ButtonStyle::Primary || info.style == ButtonStyle::Danger) {
        const bool primary = info.style == ButtonStyle::Primary;
        fill = border = primary ? palette.accent : palette.danger;
        textColour = primary ? palette.onAccent : palette.onDanger;
        borderWidth = 0.0f;
        font = boldFont_;
    }
    if (pressed) fill = Mix(fill, palette.background, 0.25);
    else if (info.hovered) fill = Mix(fill, palette.text, 0.10);

    const COLORREF surface = SurfaceColour(info.surface);
    HBRUSH surfaceBrush = ::CreateSolidBrush(surface);
    ::FillRect(item.hDC, &item.rcItem, surfaceBrush);
    ::DeleteObject(surfaceBrush);
    Gdiplus::Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float radius = static_cast<float>(Scale(6));
    RECT body = item.rcItem;
    if (focused) {
        // A ring in the text colour, clearly visible whatever the button's fill.
        RoundedRect(graphics, body, radius + Scale(2), surface, palette.text, static_cast<float>(Scale(2)));
        ::InflateRect(&body, -Scale(3), -Scale(3));
    }
    RoundedRect(graphics, body, radius, fill, border, borderWidth);

    wchar_t text[64]{};
    ::GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    ::SetBkMode(item.hDC, TRANSPARENT);
    Text(item.hDC, font, textColour, text, item.rcItem, DT_CENTER);
}

COLORREF MainWindow::SurfaceColour(Surface surface) const {
    const Palette& palette = dark_ ? kDark : kLight;
    if (surface == Surface::Window) return palette.background;
    const HandPanelView& hand = surface == Surface::LeftCard ? view_.left : view_.right;
    return hand.summary.state.tone == Tone::Accent ? palette.frozenSurface : palette.surface;
}

COLORREF MainWindow::ToneColour(Tone tone) const {
    const Palette& palette = dark_ ? kDark : kLight;
    switch (tone) {
    case Tone::Muted: return palette.muted;
    case Tone::Good: return palette.good;
    case Tone::Accent: return palette.accent;
    case Tone::Warning: return palette.warning;
    case Tone::Error: return palette.error;
    default: return palette.text;
    }
}

SIZE MainWindow::WindowSizeForClient(int clientWidthDips, int clientHeightDips) const {
    RECT rect{0, 0, Scale(clientWidthDips), Scale(clientHeightDips)};
    ::AdjustWindowRectExForDpi(&rect, kWindowStyle, FALSE, WS_EX_CONTROLPARENT, dpi_);
    return {rect.right - rect.left, rect.bottom - rect.top};
}

} // namespace vr_controller_freeze::app
