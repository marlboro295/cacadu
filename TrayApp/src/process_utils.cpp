#include "process_utils.h"

#include <cerrno>
#include <climits>
#include <cwchar>
#include <shellapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <vector>

namespace {

bool ParsePid(const wchar_t* text, DWORD& pid) {
    if (!text || !*text) return false;

    errno = 0;
    wchar_t* end = nullptr;
    unsigned long long value = wcstoull(text, &end, 10);
    if (errno == ERANGE || end == text || !end || *end != L'\0' ||
        value == 0 || value > static_cast<unsigned long long>(MAXDWORD)) {
        return false;
    }

    pid = static_cast<DWORD>(value);
    return true;
}

bool GetModulePath(std::wstring& path) {
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                      static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return false;
    path.assign(buffer.data(), length);
    return true;
}

std::wstring ExpectedServicePath() {
    std::wstring modulePath;
    if (!GetModulePath(modulePath)) return {};
    const size_t slash = modulePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return modulePath.substr(0, slash + 1) + L"TrayService.exe";
}

}

LaunchOptions ParseLaunchOptions() {
    LaunchOptions result;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        result.valid = false;
        return result;
    }

    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"/silent") == 0) {
            result.silent = true;
            continue;
        }

        if (_wcsicmp(argv[i], L"/parent-pid") == 0) {
            if (result.hasParentPid || i + 1 >= argc ||
                !ParsePid(argv[++i], result.parentPid)) {
                result.valid = false;
                break;
            }
            result.hasParentPid = true;
        }
    }

    LocalFree(argv);
    return result;
}

bool GetCurrentUserSidString(std::wstring& sid) {
    sid.clear();

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        CloseHandle(token);
        return false;
    }

    std::vector<BYTE> buffer(bytes);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        CloseHandle(token);
        return false;
    }

    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR sidText = nullptr;
    bool ok = ConvertSidToStringSidW(user->User.Sid, &sidText) != FALSE;
    if (ok && sidText) sid.assign(sidText);
    if (sidText) LocalFree(sidText);
    CloseHandle(token);
    return ok && !sid.empty();
}

DWORD GetParentProcessId(DWORD processId) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == processId) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return parent;
}

bool GetProcessImagePath(DWORD processId, std::wstring& path) {
    path.clear();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;

    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    bool ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &length) != FALSE;
    if (ok) path.assign(buffer.data(), length);
    CloseHandle(process);
    return ok && !path.empty();
}

bool ValidateServiceParent(const LaunchOptions& options,
                           DWORD currentProcessId,
                           std::wstring& errorMessage) {
    errorMessage.clear();
    if (!options.hasParentPid) {
        errorMessage = L"GUI не запущен Windows-службой.";
        return false;
    }

    DWORD actualParent = GetParentProcessId(currentProcessId);
    if (actualParent == 0 || actualParent != options.parentPid) {
        errorMessage = L"Фактический parent process не совпадает с переданным Service PID.";
        return false;
    }

    std::wstring expectedPath = ExpectedServicePath();
    if (expectedPath.empty()) {
        errorMessage = L"Невозможно определить путь TrayService.exe.";
        return false;
    }

    std::wstring parentPath;
    if (GetProcessImagePath(actualParent, parentPath) &&
        _wcsicmp(parentPath.c_str(), expectedPath.c_str()) != 0) {
        errorMessage = L"Parent process не является TrayService.exe из каталога приложения.";
        return false;
    }

    return true;
}

bool OpenValidatedServiceParent(const LaunchOptions& options,
    DWORD currentProcessId,
    HANDLE& serviceProcess,
    std::wstring& errorMessage) {
    serviceProcess = nullptr;
    if (!ValidateServiceParent(options, currentProcessId, errorMessage))
        return false;

    serviceProcess = OpenProcess(SYNCHRONIZE, FALSE, options.parentPid);
    if (!serviceProcess) {
        OutputDebugStringW(
            L"TrayApp: parent process handle unavailable; using SCM liveness fallback\n");
    }
    return true;
}