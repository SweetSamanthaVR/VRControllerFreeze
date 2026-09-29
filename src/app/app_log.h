#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace vr_controller_freeze::app {

enum class LogLevel { Info, Warn, Error };

// The Live Log. Every line is appended to
// %LOCALAPPDATA%\VRControllerFreeze\logs\VRControllerFreeze.log (rolled over to
// VRControllerFreeze.previous.log at 5 MB) and kept in a bounded copy for the window.
class AppLog {
public:
    // trimmedChars is how much of the previous text was trimmed from its start, or
    // std::wstring::npos if the text was replaced rather than extended (the log was cleared).
    using Listener = std::function<void(const std::wstring& visibleText, size_t trimmedChars)>;

    // Resolves the log folder, creating it if needed. Lines written before this go to the window only.
    void Open();
    void Write(LogLevel level, const std::wstring& message);
    // Empties both the window copy and the log file.
    void Clear();

    // Called with the full visible text, and how much was trimmed from it, after every change.
    void SetListener(Listener listener) { listener_ = std::move(listener); }
    const std::wstring& VisibleText() const { return visible_; }
    const std::filesystem::path& Directory() const { return directory_; }
    const std::filesystem::path& File() const { return file_; }
    // The log file's path for display, written with %LOCALAPPDATA% so it does not show the user's name.
    const std::wstring& DisplayFile() const { return displayFile_; }

private:
    void AppendToFile(const std::wstring& line) const;

    std::filesystem::path directory_;
    std::filesystem::path file_;
    std::wstring displayFile_;
    std::wstring visible_;
    bool replaced_ = false; // set by Clear, so the next change is reported as a replacement
    Listener listener_;
};

} // namespace vr_controller_freeze::app
