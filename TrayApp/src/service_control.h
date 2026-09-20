#pragma once

#include <windows.h>

struct TrayServiceStatus {
    DWORD state = SERVICE_STOPPED;
    DWORD win32ExitCode = ERROR_SUCCESS;
    DWORD checkPoint = 0;
};

bool QueryTrayServiceStatus(TrayServiceStatus& status);
bool StartTrayServiceAndWait(DWORD timeoutMs, DWORD* errorCode = nullptr);
