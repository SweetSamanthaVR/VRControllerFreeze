#pragma once

#ifdef _WIN32

#include "shared_state.h"
#include <windows.h>
#include <sddl.h>

#include <string>
#include <vector>

namespace vr_controller_freeze {

class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory() { Close(); }

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    // Creates the mapping, or opens it if the other side already created it. Only the creator
    // initialises the contents; an opener waits for that to finish rather than wiping live state.
    bool OpenOrCreate() {
        Close();

        PSECURITY_DESCRIPTOR descriptor = BuildSecurityDescriptor();
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, descriptor ? &attributes : nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(sizeof(SharedState)), kSharedMappingName);
        const bool created = mapping_ && ::GetLastError() != ERROR_ALREADY_EXISTS;
        if (descriptor) ::LocalFree(descriptor);
        if (!mapping_) {
            return false;
        }

        state_ = static_cast<SharedState*>(::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
        if (!state_) {
            ::CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }

        if (created) {
            Initialise();
        } else {
            // The creator may still be initialising. If it never finishes (it died mid-way), take over.
            for (int waited = 0; waited < 100 && !IsInitialised(); ++waited) ::Sleep(10);
            if (!IsInitialised()) Initialise();
        }
        return true;
    }

    void Close() {
        if (state_) {
            ::UnmapViewOfFile(state_);
            state_ = nullptr;
        }
        if (mapping_) {
            ::CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

    SharedState* Get() const { return state_; }
    explicit operator bool() const { return state_ != nullptr; }

private:
    bool IsInitialised() const {
        return ::InterlockedCompareExchange(&state_->magic, 0, 0) == kSharedMagic &&
               ::InterlockedCompareExchange(&state_->version, 0, 0) == kSharedVersion;
    }

    // Publishes magic last, so an opener that sees it knows every other field is ready.
    void Initialise() {
        ::InterlockedExchange(&state_->magic, 0);
        ::ZeroMemory(reinterpret_cast<BYTE*>(state_) + sizeof(LONG), sizeof(SharedState) - sizeof(LONG));
        ::InterlockedExchange(&state_->leftTargetDeviceIndex, kInvalidDeviceIndex);
        ::InterlockedExchange(&state_->rightTargetDeviceIndex, kInvalidDeviceIndex);
        ::InterlockedExchange(&state_->leftFrozenDeviceIndex, kInvalidDeviceIndex);
        ::InterlockedExchange(&state_->rightFrozenDeviceIndex, kInvalidDeviceIndex);
        ::InterlockedExchange(&state_->leftPoseUpdateDeviceIndex, kInvalidDeviceIndex);
        ::InterlockedExchange(&state_->rightPoseUpdateDeviceIndex, kInvalidDeviceIndex);
        ::InterlockedExchange(&state_->version, kSharedVersion);
        ::InterlockedExchange(&state_->magic, kSharedMagic);
    }

    // Grants SYSTEM, Administrators and the current user full access and sets a medium integrity
    // label, so an elevated vrserver and a non-elevated app (or the reverse) can both open the
    // mapping. Returns nullptr (default security) if the descriptor cannot be built.
    static PSECURITY_DESCRIPTOR BuildSecurityDescriptor() {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
        DWORD size = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        LPWSTR sid = nullptr;
        const bool haveSid = size > 0 && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
                             ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid);
        ::CloseHandle(token);
        if (!haveSid) return nullptr;

        const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(sid) + L")S:(ML;;NW;;;ME)";
        ::LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
            return nullptr;
        }
        return descriptor;
    }

    HANDLE mapping_ = nullptr;
    SharedState* state_ = nullptr;
};

inline LONG ReadSharedLong(volatile LONG* value) {
    return ::InterlockedCompareExchange(const_cast<LONG*>(value), 0, 0);
}

inline void WriteSharedLong(volatile LONG* value, LONG newValue) {
    ::InterlockedExchange(const_cast<LONG*>(value), newValue);
}

} // namespace vr_controller_freeze

#endif
