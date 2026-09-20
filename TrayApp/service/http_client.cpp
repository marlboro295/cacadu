#include "http_client.h"
#include "api_config.h"
#include <vector>

namespace {
constexpr size_t kMaxResponseBytes = 1024 * 1024;
}

static std::wstring WinError(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring out = msg ? msg : L"WinHTTP error";
    if (msg) LocalFree(msg);
    return out;
}

HttpResponse HttpClient::PostJson(const std::wstring& path,
                                  const std::string& json,
                                  const std::wstring& bearer) {
    return Request(L"POST", path, json, bearer);
}

HttpResponse HttpClient::GetJson(const std::wstring& path,
                                 const std::wstring& bearer) {
    return Request(L"GET", path, "", bearer);
}

HttpResponse HttpClient::Request(const wchar_t* method,
                                 const std::wstring& path,
                                 const std::string& body,
                                 const std::wstring& bearer) {
    HttpResponse result;
    HINTERNET session = WinHttpOpen(L"TrayAppService/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { result.error = WinError(GetLastError()); return result; }

    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    HINTERNET connect = WinHttpConnect(session, ApiConfig::Host().c_str(),
                                       ApiConfig::Port(), 0);
    if (!connect) {
        result.error = WinError(GetLastError());
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD flags = ApiConfig::Secure() ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, method, path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        result.error = WinError(GetLastError());
        WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
        return result;
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (!body.empty()) headers += L"Content-Type: application/json\r\n";
    if (!bearer.empty()) headers += L"Authorization: Bearer " + bearer + L"\r\n";

    BOOL ok = WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (ok) ok = WinHttpReceiveResponse(request, nullptr);

    if (!ok) {
        result.error = WinError(GetLastError());
    } else {
        DWORD size = sizeof(result.status);
        if (!WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &size,
                WINHTTP_NO_HEADER_INDEX)) {
            result.error = WinError(GetLastError());
        } else {
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    result.error = WinError(GetLastError());
                    break;
                }
                if (available == 0) break;
                if (available > kMaxResponseBytes - result.body.size()) {
                    result.error = L"HTTP response exceeds the size limit";
                    break;
                }
                std::vector<char> chunk(available);
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read)) {
                    result.error = WinError(GetLastError());
                    break;
                }
                result.body.append(chunk.data(), read);
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}
