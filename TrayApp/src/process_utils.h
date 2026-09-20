#pragma once

#include <windows.h>
#include <string>

struct LaunchOptions {
    bool valid = true;
    bool silent = false;
    bool hasParentPid = false;
    DWORD parentPid = 0;
};

LaunchOptions ParseLaunchOptions();
bool GetCurrentUserSidString(std::wstring& sid);
DWORD GetParentProcessId(DWORD processId);
bool GetProcessImagePath(DWORD processId, std::wstring& path);
bool ValidateServiceParent(const LaunchOptions& options,
                           DWORD currentProcessId,
                           std::wstring& errorMessage);
bool OpenValidatedServiceParent(const LaunchOptions& options,
                                DWORD currentProcessId,
                                HANDLE& serviceProcess,
                                std::wstring& errorMessage);
