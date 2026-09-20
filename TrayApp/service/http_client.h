#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>

struct HttpResponse {
    DWORD status = 0;
    std::string body;
    std::wstring error;
    bool TransportOk() const { return error.empty(); }
    bool Ok() const { return TransportOk() && status >= 200 && status < 300; }
};

class HttpClient {
public:
    static HttpResponse PostJson(const std::wstring& path,
                                 const std::string& json,
                                 const std::wstring& bearer = L"");
    static HttpResponse GetJson(const std::wstring& path,
                                const std::wstring& bearer = L"");
private:
    static HttpResponse Request(const wchar_t* method,
                                const std::wstring& path,
                                const std::string& body,
                                const std::wstring& bearer);
};
