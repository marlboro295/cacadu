#include "session.h"
#include <userenv.h>
#include <wtsapi32.h>
#include <mutex>
#include <vector>

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "wtsapi32.lib")

struct LaunchedProcess {
    HANDLE process = nullptr;
    DWORD pid = 0;
    DWORD sessionId = 0;
};

static std::mutex g_processMutex;
static std::vector<LaunchedProcess> g_launchedProcesses;
static bool g_acceptLaunches = true;

static void PruneExitedProcessesLocked() {
    for (auto it = g_launchedProcesses.begin();
         it != g_launchedProcesses.end();) {
        const DWORD waitResult = WaitForSingleObject(it->process, 0);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED) {
            CloseHandle(it->process);
            it = g_launchedProcesses.erase(it);
        } else {
            ++it;
        }
    }
}

DWORD LaunchInSession(DWORD sessionId, const std::wstring& exePath) {
    if (exePath.empty()) return 0;

    {
        std::lock_guard<std::mutex> lock(g_processMutex);
        PruneExitedProcessesLocked();
        if (!g_acceptLaunches) return 0;
        for (const auto& launched : g_launchedProcesses) {
            if (launched.sessionId == sessionId)
                return launched.pid;
        }
    }

    HANDLE hToken = NULL;
    if (!WTSQueryUserToken(sessionId, &hToken)) {
        return 0;
    }

    LPVOID lpEnv = NULL;
    if (!CreateEnvironmentBlock(&lpEnv, hToken, FALSE)) {
        if (lpEnv) DestroyEnvironmentBlock(lpEnv);
        CloseHandle(hToken);
        return 0;
    }

    std::wstring commandLine = L"\"" + exePath + L"\" /parent-pid " +
                               std::to_wstring(GetCurrentProcessId()) +
                               L" /silent";
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(),
                                             commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessAsUserW(
        hToken, NULL, mutableCommandLine.data(),
        NULL, NULL, FALSE,
        CREATE_UNICODE_ENVIRONMENT | NORMAL_PRIORITY_CLASS,
        lpEnv, NULL, &si, &pi
    );

    if (lpEnv) DestroyEnvironmentBlock(lpEnv);
    CloseHandle(hToken);

    if (!ok) return 0;

    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);

    bool accepted = false;
    DWORD existingPid = 0;
    {
        std::lock_guard<std::mutex> lock(g_processMutex);
        PruneExitedProcessesLocked();
        if (g_acceptLaunches) {
            for (const auto& launched : g_launchedProcesses) {
                if (launched.sessionId == sessionId) {
                    existingPid = launched.pid;
                    break;
                }
            }
            if (existingPid == 0) {
                g_launchedProcesses.push_back({pi.hProcess, pid, sessionId});
                accepted = true;
            }
        }
    }

    if (!accepted) {
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        return existingPid;
    }
    return pid;
}

void PruneExitedProcesses() {
    std::lock_guard<std::mutex> lock(g_processMutex);
    PruneExitedProcessesLocked();
}

bool LaunchInAllActiveSessions(const std::wstring& exePath) {
    PWTS_SESSION_INFOW pSessions = NULL;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessions, &count))
        return false;

    bool allSucceeded = true;
    for (DWORD i = 0; i < count; ++i) {
        DWORD sid = pSessions[i].SessionId;
        if (sid == 0) continue;
        if (pSessions[i].State != WTSActive) continue;
        if (LaunchInSession(sid, exePath) == 0)
            allSucceeded = false;
    }
    WTSFreeMemory(pSessions);
    return allSucceeded;
}

void TerminateAllLaunched() {
    std::vector<LaunchedProcess> processes;
    {
        std::lock_guard<std::mutex> lock(g_processMutex);
        PruneExitedProcessesLocked();
        g_acceptLaunches = false;
        processes.swap(g_launchedProcesses);
    }

    for (const LaunchedProcess& process : processes) {
        if (!process.process) continue;
        TerminateProcess(process.process, 0);
        WaitForSingleObject(process.process, 5000);
        CloseHandle(process.process);
    }
}
