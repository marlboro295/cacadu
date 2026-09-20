#pragma once
#include <windows.h>
#include <winhttp.h>
#include <cstdlib>
#include <iterator>
#include <string>

namespace ApiConfig {
inline std::wstring Env(const wchar_t* name, const wchar_t* fallback) {
    wchar_t buffer[2048]{};
    DWORD n = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n > 0 && n < std::size(buffer)) return buffer;
    return fallback;
}

// Change these defaults to the exact HTTPS server from assignment 1.2,
// or set the corresponding host/path environment variables on TrayAppService.
inline std::wstring Host() { return Env(L"TRAY_API_HOST", L"localhost"); }
inline INTERNET_PORT Port() {
    std::wstring v = Env(L"TRAY_API_PORT", L"8443");
    int p = _wtoi(v.c_str());
    return p > 0 && p <= 65535 ? static_cast<INTERNET_PORT>(p) : 8443;
}
inline bool Secure() { return true; }

inline std::wstring LoginPath()    { return Env(L"TRAY_API_LOGIN_PATH",    L"/api/auth/login"); }
inline std::wstring RefreshPath()  { return Env(L"TRAY_API_REFRESH_PATH",  L"/api/auth/refresh"); }
inline std::wstring LicensePath()  { return Env(L"TRAY_API_LICENSE_PATH",  L"/api/license"); }
inline std::wstring ActivatePath() { return Env(L"TRAY_API_ACTIVATE_PATH", L"/api/license/activate"); }
}
