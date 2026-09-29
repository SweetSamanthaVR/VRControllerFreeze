#include "app_log.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <system_error>

#include "win_util.h"

namespace vr_controller_freeze::app {
namespace {

constexpr std::uintmax_t kMaxLogFileBytes = 5u * 1024u * 1024u;
constexpr size_t kMaxVisibleChars = 60000;

std::wstring Timestamp() {
    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    wchar_t text[32]{};
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", static_cast<unsigned>(t.wYear), static_cast<unsigned>(t.wMonth),
               static_cast<unsigned>(t.wDay), static_cast<unsigned>(t.wHour), static_cast<unsigned>(t.wMinute),
               static_cast<unsigned>(t.wSecond), static_cast<unsigned>(t.wMilliseconds));
    return text;
}

const wchar_t* LevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Warn: return L"WARN";
    case LogLevel::Error: return L"ERROR";
    default: return L"INFO";
    }
}

HANDLE OpenForAppend(const std::filesystem::path& file) {
    return ::CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

void AppLog::Open() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    const bool haveLocalAppData = length > 0 && length < buffer.size();
    const std::filesystem::path base = haveLocalAppData ? std::filesystem::path(std::wstring(buffer.data(), length)) : ExecutableDirectory();
    directory_ = base / L"VRControllerFreeze" / L"logs";
    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);
    file_ = directory_ / L"VRControllerFreeze.log";
    displayFile_ = haveLocalAppData ? L"%LOCALAPPDATA%\\VRControllerFreeze\\logs\\VRControllerFreeze.log" : file_.wstring();
}

void AppLog::Write(LogLevel level, const std::wstring& message) {
    const std::wstring line = Timestamp() + L" [" + LevelName(level) + L"] " + message;
    AppendToFile(line);
    visible_ += line + L"\r\n";
    size_t trimmed = replaced_ ? std::wstring::npos : 0;
    replaced_ = false;
    if (visible_.size() > kMaxVisibleChars) {
        const size_t cut = visible_.find(L'\n', visible_.size() - kMaxVisibleChars);
        if (cut != std::wstring::npos) {
            visible_.erase(0, cut + 1);
            if (trimmed != std::wstring::npos) trimmed = cut + 1;
        }
    }
    if (listener_) listener_(visible_, trimmed);
}

void AppLog::Clear() {
    visible_.clear();
    replaced_ = true;
    if (!file_.empty()) {
        HANDLE file = ::CreateFileW(file_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
    }
    Write(LogLevel::Info, L"Log cleared.");
}

// Appends one line, rolling over to VRControllerFreeze.previous.log once the file passes kMaxLogFileBytes.
void AppLog::AppendToFile(const std::wstring& line) const {
    if (file_.empty()) return;
    const std::string bytes = Utf8(line + L"\r\n");
    if (bytes.empty()) return;
    HANDLE file = OpenForAppend(file_);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) && static_cast<std::uintmax_t>(size.QuadPart) > kMaxLogFileBytes) {
        ::CloseHandle(file);
        std::error_code ignored;
        const std::filesystem::path previous = directory_ / L"VRControllerFreeze.previous.log";
        std::filesystem::remove(previous, ignored);
        std::filesystem::rename(file_, previous, ignored);
        file = OpenForAppend(file_);
        if (file == INVALID_HANDLE_VALUE) return;
    }
    DWORD written = 0;
    ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    ::CloseHandle(file);
}

} // namespace vr_controller_freeze::app
