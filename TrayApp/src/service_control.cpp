#include "service_control.h"

namespace {

constexpr wchar_t kServiceName[] = L"TrayAppService";

bool ReadStatus(SC_HANDLE service, TrayServiceStatus& result) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)) {
        return false;
    }

    result.state = status.dwCurrentState;
    result.win32ExitCode = status.dwWin32ExitCode;
    result.checkPoint = status.dwCheckPoint;
    return true;
}

void SetErrorCode(DWORD* errorCode, DWORD value) {
    if (errorCode) *errorCode = value;
    SetLastError(value);
}

}

bool QueryTrayServiceStatus(TrayServiceStatus& status) {
    status = {};

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;

    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (!service) {
        DWORD error = GetLastError();
        CloseServiceHandle(manager);
        SetLastError(error);
        return false;
    }

    bool ok = ReadStatus(service, status);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!ok) SetLastError(error);
    return ok;
}

bool StartTrayServiceAndWait(DWORD timeoutMs, DWORD* errorCode) {
    if (errorCode) *errorCode = ERROR_SUCCESS;

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        if (errorCode) *errorCode = GetLastError();
        return false;
    }

    SC_HANDLE service = OpenServiceW(manager, kServiceName,
                                     SERVICE_QUERY_STATUS | SERVICE_START);
    if (!service) {
        DWORD error = GetLastError();
        CloseServiceHandle(manager);
        if (errorCode) *errorCode = error;
        SetLastError(error);
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool startIssued = false;
    for (;;) {
        TrayServiceStatus status{};
        if (!ReadStatus(service, status)) {
            DWORD error = GetLastError();
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            if (errorCode) *errorCode = error;
            SetLastError(error);
            return false;
        }

        if (status.state == SERVICE_RUNNING) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return true;
        }

        if (status.state == SERVICE_STOPPED) {
            if (startIssued) {
                DWORD error = status.win32ExitCode != ERROR_SUCCESS
                    ? status.win32ExitCode : ERROR_SERVICE_NOT_ACTIVE;
                CloseServiceHandle(service);
                CloseServiceHandle(manager);
                SetErrorCode(errorCode, error);
                return false;
            }

            if (!StartServiceW(service, 0, nullptr)) {
                DWORD error = GetLastError();
                if (error != ERROR_SERVICE_ALREADY_RUNNING) {
                    CloseServiceHandle(service);
                    CloseServiceHandle(manager);
                    SetErrorCode(errorCode, error);
                    return false;
                }
            }
            startIssued = true;
        } else if (status.state != SERVICE_START_PENDING &&
                   status.state != SERVICE_STOP_PENDING) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            SetErrorCode(errorCode, ERROR_INVALID_STATE);
            return false;
        }

        if (GetTickCount64() >= deadline) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            SetErrorCode(errorCode, ERROR_TIMEOUT);
            return false;
        }
        Sleep(100);
    }
}
