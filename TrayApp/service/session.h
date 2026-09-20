#pragma once
#include <windows.h>
#include <string>

DWORD LaunchInSession(DWORD sessionId, const std::wstring& exePath);
bool LaunchInAllActiveSessions(const std::wstring& exePath);
void PruneExitedProcesses();
void TerminateAllLaunched();
