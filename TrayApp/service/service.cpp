#include <windows.h>
#include <wtsapi32.h>
#include <vector>
#include <string>
#include "session.h"
#include "rpc.h"
#include "auth_manager.h"
#include "license_manager.h"
#include "antivirus_manager.h"

static SERVICE_STATUS_HANDLE g_hStatusHandle=nullptr;
static SERVICE_STATUS g_Status={};
static HANDLE g_hStopEvent=nullptr;
static HANDLE g_hRpcThread=nullptr;
static HANDLE g_hRpcReadyEvent=nullptr;
static std::wstring g_exePath;
static wchar_t g_serviceName[] = L"TrayAppService";

void WINAPI ServiceMain(DWORD,LPWSTR*);
DWORD WINAPI ServiceCtrlHandlerEx(DWORD,DWORD,LPVOID,LPVOID);
DWORD WINAPI RpcServerThread(LPVOID);

static std::wstring GetGuiExePath(){
    std::vector<wchar_t> buffer(512);
    for (;;) {
        DWORD length=GetModuleFileNameW(nullptr,buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
        if (length==0) return {};
        if (length < buffer.size()-1) {
            std::wstring servicePath(buffer.data(),length);
            size_t pos=servicePath.find_last_of(L"\\/");
            if (pos==std::wstring::npos) return {};
            return servicePath.substr(0,pos+1)+L"TrayApp.exe";
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size()*2);
    }
}

int wmain(int,wchar_t**){
    SERVICE_TABLE_ENTRYW table[]={
        {g_serviceName,ServiceMain},{nullptr,nullptr}};
    return StartServiceCtrlDispatcherW(table)?0:1;
}

void WINAPI ServiceMain(DWORD,LPWSTR*){
    g_hStatusHandle=RegisterServiceCtrlHandlerExW(g_serviceName,ServiceCtrlHandlerEx,nullptr);
    if(!g_hStatusHandle) return;

    g_Status.dwServiceType=SERVICE_WIN32_OWN_PROCESS;
    g_Status.dwCurrentState=SERVICE_START_PENDING;
    g_Status.dwControlsAccepted=0;
    g_Status.dwWin32ExitCode=NO_ERROR;
    SetServiceStatus(g_hStatusHandle,&g_Status);

    g_exePath=GetGuiExePath();
    DWORD guiPathError=ERROR_SUCCESS;
    if(g_exePath.empty()){
        guiPathError=ERROR_FILE_NOT_FOUND;
    }else if(GetFileAttributesW(g_exePath.c_str()) == INVALID_FILE_ATTRIBUTES){
        guiPathError=GetLastError();
    }
    if(guiPathError!=ERROR_SUCCESS){
        g_Status.dwCurrentState=SERVICE_STOPPED;
        g_Status.dwWin32ExitCode=guiPathError;
        SetServiceStatus(g_hStatusHandle,&g_Status);
        return;
    }

    g_hStopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!g_hStopEvent){
        g_Status.dwCurrentState=SERVICE_STOPPED;
        g_Status.dwWin32ExitCode=GetLastError();
        SetServiceStatus(g_hStatusHandle,&g_Status); return;
    }

    AuthManager::Instance().Start();
    LicenseManager::Instance().Start();
    AntivirusManager::Instance().Start();
    g_hRpcReadyEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!g_hRpcReadyEvent){
        AntivirusManager::Instance().Stop();
        LicenseManager::Instance().Stop();
        AuthManager::Instance().Stop();
        DWORD error=GetLastError();
        CloseHandle(g_hStopEvent); g_hStopEvent=nullptr;
        g_Status.dwCurrentState=SERVICE_STOPPED;
        g_Status.dwWin32ExitCode=error;
        SetServiceStatus(g_hStatusHandle,&g_Status);
        return;
    }
    g_hRpcThread=CreateThread(nullptr,0,RpcServerThread,nullptr,0,nullptr);
    if(!g_hRpcThread){
        AntivirusManager::Instance().Stop();
        LicenseManager::Instance().Stop();
        AuthManager::Instance().Stop();
        DWORD error=GetLastError();
        CloseHandle(g_hRpcReadyEvent); g_hRpcReadyEvent=nullptr;
        CloseHandle(g_hStopEvent); g_hStopEvent=nullptr;
        g_Status.dwCurrentState=SERVICE_STOPPED;
        g_Status.dwWin32ExitCode=error;
        SetServiceStatus(g_hStatusHandle,&g_Status);
        return;
    }

    if(WaitForSingleObject(g_hRpcReadyEvent,5000)!=WAIT_OBJECT_0 ||
       !IsRpcServerReady()){
        SetEvent(g_hStopEvent);
        StopRpcServer();
        WaitForSingleObject(g_hRpcThread,INFINITE);
        CloseHandle(g_hRpcThread); g_hRpcThread=nullptr;
        CloseHandle(g_hRpcReadyEvent); g_hRpcReadyEvent=nullptr;
        CloseHandle(g_hStopEvent); g_hStopEvent=nullptr;
        AntivirusManager::Instance().Stop();
        LicenseManager::Instance().Stop();
        AuthManager::Instance().Stop();
        g_Status.dwCurrentState=SERVICE_STOPPED;
        g_Status.dwWin32ExitCode=ERROR_TIMEOUT;
        SetServiceStatus(g_hStatusHandle,&g_Status);
        return;
    }

    if(!LaunchInAllActiveSessions(g_exePath))
        OutputDebugStringW(L"TrayService: one or more session GUI launches failed\n");

    g_Status.dwCurrentState=SERVICE_RUNNING;
    g_Status.dwControlsAccepted=SERVICE_ACCEPT_SESSIONCHANGE;
    g_Status.dwWin32ExitCode=NO_ERROR;
    SetServiceStatus(g_hStatusHandle,&g_Status);

    WaitForSingleObject(g_hStopEvent,INFINITE);
    g_Status.dwCurrentState=SERVICE_STOP_PENDING;
    g_Status.dwControlsAccepted=0;
    SetServiceStatus(g_hStatusHandle,&g_Status);

    // Stop accepting new RPC calls first, then wait until the RPC thread has
    // unregistered the interface and all in-flight calls have drained.
    StopRpcServer();
    if(g_hRpcThread){
        WaitForSingleObject(g_hRpcThread,INFINITE);
        CloseHandle(g_hRpcThread); g_hRpcThread=nullptr;
    }
    TerminateAllLaunched();
    AntivirusManager::Instance().Stop();
    LicenseManager::Instance().Stop();
    AuthManager::Instance().Stop();
    if(g_hRpcReadyEvent){
        CloseHandle(g_hRpcReadyEvent); g_hRpcReadyEvent=nullptr;
    }
    if(g_hStopEvent){
        CloseHandle(g_hStopEvent); g_hStopEvent=nullptr;
    }
    g_Status.dwCurrentState=SERVICE_STOPPED;
    SetServiceStatus(g_hStatusHandle,&g_Status);
}

DWORD WINAPI ServiceCtrlHandlerEx(DWORD control,DWORD eventType,LPVOID eventData,LPVOID){
    if(control==SERVICE_CONTROL_STOP || control==SERVICE_CONTROL_SHUTDOWN)
        return ERROR_CALL_NOT_IMPLEMENTED;
    if(control==SERVICE_CONTROL_SESSIONCHANGE)
        PruneExitedProcesses();
    if(control==SERVICE_CONTROL_SESSIONCHANGE && eventType==WTS_SESSION_LOGON){
        auto* n=(WTSSESSION_NOTIFICATION*)eventData;
        if(n) LaunchInSession(n->dwSessionId,g_exePath);
    }
    return NO_ERROR;
}
DWORD WINAPI RpcServerThread(LPVOID){
    const bool ok=StartRpcServer(g_hStopEvent,g_hRpcReadyEvent);
    if(!ok && g_hStopEvent) SetEvent(g_hStopEvent);
    return ok ? 0 : 1;
}
