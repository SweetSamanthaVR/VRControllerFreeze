#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <unordered_map>

#include "hand_logic.h"

namespace vr_controller_freeze::app {

enum class Command { ToggleLeft, ToggleRight, ToggleBoth, EmergencyRelease, Bindings, Diagnostics, OpenLogFolder, ClearLog, ToggleTheme };

// One hand's card.
struct HandPanelView {
    HandSummary summary;
    std::wstring device;
    Summary tracking;
    bool held = false;       // the card's button unfreezes rather than freezes
    bool canFreeze = false;  // driver, hook and controller are all ready; otherwise the button is shown quiet
    bool operator==(const HandPanelView&) const = default;
};

// Everything the window shows apart from the log.
struct WindowView {
    Summary steamVr;
    Summary driver;
    Summary hook;
    HandPanelView left;
    HandPanelView right;
    bool operator==(const WindowView&) const = default;
};

// The main window: a header, status pills, a card per hand with its own freeze button, the
// shared actions, and the Live Log. The layout is defined in 96-DPI units and scaled for whichever
// monitor the window is on (the app manifest declares per-monitor DPI awareness), and the window
// can be resized, with the log taking up the extra space. It has a dark and a light theme; the
// status, cards and buttons are drawn by the window so both themes look the same everywhere.
class MainWindow {
public:
    struct Callbacks {
        std::function<void()> onTick;             // every kTickMs
        std::function<void(Command)> onCommand;   // a button was clicked
    };
    static constexpr UINT kTickMs = 25;

    MainWindow() = default;
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    ~MainWindow();

    bool Create(HINSTANCE instance, const std::wstring& title, bool darkMode, Callbacks callbacks);
    void Show(int showCommand);
    // Runs the message loop until the window closes and returns the exit code.
    int RunMessageLoop();
    HWND Handle() const { return hwnd_; }

    void SetView(const WindowView& view);
    // trimmedChars: see AppLog::Listener.
    void SetLog(const std::wstring& text, size_t trimmedChars);
    void SetDarkMode(bool dark);

private:
    // Quiet: a secondary button with muted text, for an action that cannot work right now.
    enum class ButtonStyle { Primary, Secondary, Quiet, Danger };
    enum class Surface { Window, LeftCard, RightCard };
    struct ButtonInfo {
        ButtonStyle style = ButtonStyle::Secondary;
        Surface surface = Surface::Window;
        bool hovered = false;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ButtonProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR self);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    HWND CreateButton(const wchar_t* text, int id, ButtonStyle style, Surface surface);
    void CreateControls();
    void ApplyDpi(UINT dpi);
    void ApplyTheme();
    void Layout();
    void UpdateCardButtons();
    void Paint(HDC dc);
    void PaintHandCard(HDC dc, const RECT& card, const wchar_t* title, const HandPanelView& hand, Surface surface);
    void DrawButton(const DRAWITEMSTRUCT& item);
    COLORREF SurfaceColour(Surface surface) const;
    COLORREF ToneColour(Tone tone) const;
    int Scale(int dips) const { return ::MulDiv(dips, static_cast<int>(dpi_), 96); }
    SIZE WindowSizeForClient(int clientWidthDips, int clientHeightDips) const;

    HWND hwnd_ = nullptr;
    HWND leftButton_ = nullptr;
    HWND rightButton_ = nullptr;
    HWND bothButton_ = nullptr;
    HWND bindingsButton_ = nullptr;
    HWND releaseButton_ = nullptr;
    HWND log_ = nullptr;
    HWND diagnosticsButton_ = nullptr;
    HWND openLogButton_ = nullptr;
    HWND clearLogButton_ = nullptr;
    HWND themeButton_ = nullptr;
    HWND lastFocus_ = nullptr; // restored when the window is activated again
    std::unordered_map<HWND, ButtonInfo> buttons_;

    // Fonts, recreated for each DPI.
    HFONT uiFont_ = nullptr;
    HFONT boldFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT stateFont_ = nullptr;
    HFONT captionFont_ = nullptr;
    HFONT logFont_ = nullptr;
    int lineHeight_ = 16;
    int titleHeight_ = 24;
    int stateHeight_ = 24;
    int captionHeight_ = 14;
    UINT dpi_ = 96;

    // Layout results, in client pixels.
    RECT leftCard_{};
    RECT rightCard_{};
    int pillsTop_ = 0;
    int logLabelTop_ = 0;

    bool dark_ = true;
    HBRUSH logBrush_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
    Callbacks callbacks_;
    WindowView view_;
};

} // namespace vr_controller_freeze::app
