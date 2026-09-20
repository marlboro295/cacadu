#include "rpc.h"
#include "tray_h.h"
#include "auth_manager.h"
#include "license_manager.h"
#include "antivirus_manager.h"
#include "rpc_security.h"

#include <cstdlib>
#include <cwchar>
#include <atomic>

enum {
    APP_OK = 0,
    APP_UNAUTHORIZED = 1,
    APP_BAD_RESPONSE = 3,
    APP_NO_LICENSE = 4,
    APP_RPC_ERROR = 100
};

static HANDLE g_hStopEvent = nullptr;
static std::atomic<bool> g_rpcRegistered{false};
static std::atomic<bool> g_rpcReady{false};

extern "C" boolean StopService(handle_t binding) {
    RpcSecurity::CallerIdentity caller;
    if (!RpcSecurity::GetCallerIdentity(binding, caller) ||
        caller.sessionId == 0 ||
        !g_hStopEvent) {
        return FALSE;
    }
    return SetEvent(g_hStopEvent) ? TRUE : FALSE;
}

extern "C" long GetCurrentUser(handle_t binding, boolean* authenticated,
                               unsigned long usernameCapacity,
                               wchar_t* username) {
    if (!authenticated || !username || usernameCapacity == 0)
        return APP_BAD_RESPONSE;

    RpcSecurity::CallerIdentity caller;
    if (!RpcSecurity::GetCallerIdentity(binding, caller) ||
        caller.sessionId == 0)
        return APP_RPC_ERROR;

    auto state = AuthManager::Instance().PublicState(caller.sid);
    *authenticated = state.authenticated ? TRUE : FALSE;
    wcsncpy_s(username, usernameCapacity, state.username.c_str(), _TRUNCATE);
    return APP_OK;
}

extern "C" long Login(handle_t binding, const wchar_t* username,
                      const wchar_t* password) {
    if (!username || !password || !*username || !*password)
        return APP_BAD_RESPONSE;

    RpcSecurity::CallerIdentity caller;
    if (!RpcSecurity::GetCallerIdentity(binding, caller) ||
        caller.sessionId == 0)
        return APP_RPC_ERROR;

    const long result =
        AuthManager::Instance().Login(caller.sid, username, password);
    AntivirusManager::Instance().Reevaluate(caller.sid);
    return result;
}

extern "C" long Logout(handle_t binding) {
    RpcSecurity::CallerIdentity caller;
    if (!RpcSecurity::GetCallerIdentity(binding, caller) ||
        caller.sessionId == 0)
        return APP_RPC_ERROR;

    const long result = AuthManager::Instance().Logout(caller.sid);
    AntivirusManager::Instance().Reevaluate(caller.sid);
    return result;
}

extern "C" long GetLicenseInfo(handle_t binding, boolean* licensed,
                               __int64* expiresUnix) {
    if (!licensed || !expiresUnix)
        return APP_BAD_RESPONSE;

    RpcSecurity::CallerIdentity caller;
    if (!RpcSecurity::GetCallerIdentity(binding, caller) ||
        caller.sessionId == 0)
        return APP_RPC_ERROR;

    if (!AuthManager::Instance().PublicState(caller.sid).authenticated) {
        *licensed = FALSE;
        *expiresUnix = 0;
        return APP_UNAUTHORIZED;
    }

    auto state = LicenseManager::Instance().PublicState(caller.sid);
    *licensed = state.licensed ? TRUE : FALSE;
    *expiresUnix = state.expiresUnix;
    return state.licensed ? APP_OK : APP_NO_LICENSE;
}

extern "C" long GetAntivirusStatus(handle_t binding, boolean* running) {
    if (!running) return APP_BAD_RESPONSE;
    *running = FALSE;

    RpcSecurity::CallerIdentity caller;
    if (!RpcSecurity::GetCallerIdentity(binding, caller) ||
        caller.sessionId == 0)
        return APP_RPC_ERROR;

    bool state = false;
    const long result = AntivirusManager::Instance().GetStatus(caller.sid, state);
    *running = state ? TRUE : FALSE;
    return result;
}

extern "C" long ActivateProduct(handle_t binding,
                                const wchar_t* activationCode) {
    if (!activationCode || !*activationCode)
        return APP_BAD_RESPONSE;

    RpcSecurity::CallerIdentity caller;
    if (!RpcSecurity::GetCallerIdentity(binding, caller) ||
        caller.sessionId == 0)
        return APP_RPC_ERROR;

    const long result =
        LicenseManager::Instance().Activate(caller.sid, activationCode);
    AntivirusManager::Instance().Reevaluate(caller.sid);
    return result;
}

bool StartRpcServer(HANDLE hStopEvent, HANDLE hReadyEvent) {
    g_hStopEvent = hStopEvent;
    g_rpcRegistered.store(false);
    g_rpcReady.store(false);

    bool listening = false;
    auto fail = [hReadyEvent, &listening]() {
        g_rpcReady.store(false);
        if (listening) {
            const RPC_STATUS stopStatus = RpcMgmtStopServerListening(nullptr);
            if (stopStatus != RPC_S_OK && stopStatus != RPC_S_NOT_LISTENING)
                OutputDebugStringW(
                    L"TrayService: RPC startup stop failed\n");
            const RPC_STATUS waitStatus = RpcMgmtWaitServerListen();
            if (waitStatus != RPC_S_OK && waitStatus != RPC_S_NOT_LISTENING)
                OutputDebugStringW(
                    L"TrayService: RPC startup wait failed\n");
            listening = false;
        }
        if (g_rpcRegistered.load()) {
            const RPC_STATUS unregisterStatus =
                RpcServerUnregisterIf(TrayRpc_v1_0_s_ifspec, nullptr, TRUE);
            if (unregisterStatus != RPC_S_OK &&
                unregisterStatus != RPC_S_UNKNOWN_IF) {
                OutputDebugStringW(
                    L"TrayService: RpcServerUnregisterIf failed\n");
            }
            g_rpcRegistered.store(false);
        }
        if (hReadyEvent && !SetEvent(hReadyEvent))
            OutputDebugStringW(L"TrayService: ready event signal failed\n");
        return false;
    };

    RPC_STATUS status = RpcServerUseProtseqEpW(
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"TrayRpcEndpoint")),
        nullptr);
    if (status != RPC_S_OK)
        return fail();

    status = RpcServerRegisterIf2(
        TrayRpc_v1_0_s_ifspec,
        nullptr,
        nullptr,
        RPC_IF_ALLOW_LOCAL_ONLY | RPC_IF_ALLOW_SECURE_ONLY,
        RPC_C_LISTEN_MAX_CALLS_DEFAULT,
        static_cast<unsigned>(-1),
        nullptr);
    if (status != RPC_S_OK)
        return fail();

    g_rpcRegistered.store(true);

    status = RpcServerRegisterAuthInfoW(
        nullptr, RPC_C_AUTHN_WINNT, nullptr, nullptr);
    if (status != RPC_S_OK)
        return fail();
    if (hStopEvent && WaitForSingleObject(hStopEvent, 0) == WAIT_OBJECT_0)
        return fail();

    status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    if (status != RPC_S_OK)
        return fail();
    listening = true;
    if (hStopEvent && WaitForSingleObject(hStopEvent, 0) == WAIT_OBJECT_0)
        return fail();

    // RpcServerListen(..., TRUE) only starts the listener and returns. Keep
    // this thread alive until the listener is explicitly stopped.
    g_rpcReady.store(true);
    if (hReadyEvent && !SetEvent(hReadyEvent))
        return fail();

    const RPC_STATUS waitStatus = RpcMgmtWaitServerListen();
    listening = false;
    g_rpcReady.store(false);

    const RPC_STATUS unregisterStatus =
        RpcServerUnregisterIf(TrayRpc_v1_0_s_ifspec, nullptr, TRUE);
    g_rpcRegistered.store(false);
    if (waitStatus != RPC_S_OK || unregisterStatus != RPC_S_OK) {
        OutputDebugStringW(L"TrayService: RPC listener shutdown failed\n");
        return false;
    }
    return true;
}

bool IsRpcServerReady() {
    return g_rpcReady.load();
}

void StopRpcServer() {
    if (!g_rpcRegistered.load()) return;
    const RPC_STATUS status = RpcMgmtStopServerListening(nullptr);
    if (status != RPC_S_OK && status != RPC_S_NOT_LISTENING) {
        OutputDebugStringW(
            L"TrayService: RpcMgmtStopServerListening failed\n");
    }
}

extern "C" void* __RPC_USER MIDL_user_allocate(size_t size) {
    return malloc(size);
}

extern "C" void __RPC_USER MIDL_user_free(void* ptr) {
    free(ptr);
}
