#include "rpc_client.h"
#include "tray_h.h"

#include <windows.h>
#include <rpc.h>
#include <cstdlib>
#include <mutex>

static RPC_BINDING_HANDLE g_binding = nullptr;
static std::mutex g_bindingMutex;


// ------------------------------------------------------------
// RPC connection
// ------------------------------------------------------------

static void InvalidateBindingLocked()
{
    if (!g_binding) return;
    const RPC_STATUS status = RpcBindingFree(&g_binding);
    if (status != RPC_S_OK)
        OutputDebugStringW(L"TrayApp: RpcBindingFree failed\n");
    g_binding = nullptr;
}

static bool EnsureBindingLocked()
{
    if (g_binding)
        return true;

    RPC_WSTR bindingString = nullptr;

    RPC_STATUS status = RpcStringBindingComposeW(
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"TrayRpcEndpoint")),
        nullptr,
        &bindingString
    );

    if (status != RPC_S_OK)
        return false;

    RPC_BINDING_HANDLE binding = nullptr;
    status = RpcBindingFromStringBindingW(bindingString, &binding);
    const RPC_STATUS freeStatus = RpcStringFreeW(&bindingString);
    if (status != RPC_S_OK || freeStatus != RPC_S_OK) {
        if (binding && RpcBindingFree(&binding) != RPC_S_OK)
            OutputDebugStringW(L"TrayApp: RpcBindingFree failed\n");
        return false;
    }

    status = RpcBindingSetAuthInfoW(
        binding,
        nullptr,
        RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
        RPC_C_AUTHN_WINNT,
        nullptr,
        RPC_C_AUTHZ_NONE
    );
    if (status != RPC_S_OK)
    {
        if (RpcBindingFree(&binding) != RPC_S_OK)
            OutputDebugStringW(L"TrayApp: RpcBindingFree failed\n");
        return false;
    }

    g_binding = binding;
    return true;
}

bool RpcConnect()
{
    std::lock_guard<std::mutex> lock(g_bindingMutex);
    return EnsureBindingLocked();
}


void RpcDisconnect()
{
    std::lock_guard<std::mutex> lock(g_bindingMutex);
    InvalidateBindingLocked();
}


// ------------------------------------------------------------
// Низкоуровневые RPC-вызовы
// ------------------------------------------------------------

static long CallGetCurrentUser(
    boolean* authenticated,
    unsigned long usernameCapacity,
    wchar_t* username)
{
    long result = APP_RPC_ERROR;

    RpcTryExcept
    {
        result = GetCurrentUser(
            g_binding,
            authenticated,
            usernameCapacity,
            username
        );
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        result = APP_RPC_ERROR;
        InvalidateBindingLocked();
    }
    RpcEndExcept

        return result;
}


static long CallLogin(
    const wchar_t* username,
    const wchar_t* password)
{
    long result = APP_RPC_ERROR;

    RpcTryExcept
    {
        result = Login(
            g_binding,
            username,
            password
        );
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        result = APP_RPC_ERROR;
        InvalidateBindingLocked();
    }
    RpcEndExcept

        return result;
}


static long CallLogout()
{
    long result = APP_RPC_ERROR;

    RpcTryExcept
    {
        result = Logout(g_binding);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        result = APP_RPC_ERROR;
        InvalidateBindingLocked();
    }
    RpcEndExcept

        return result;
}


static long CallGetLicenseInfo(
    boolean* licensed,
    __int64* expiresUnix)
{
    long result = APP_RPC_ERROR;

    RpcTryExcept
    {
        result = GetLicenseInfo(
            g_binding,
            licensed,
            expiresUnix
        );
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        result = APP_RPC_ERROR;
        InvalidateBindingLocked();
    }
    RpcEndExcept

        return result;
}


static long CallActivateProduct(
    const wchar_t* activationCode)
{
    long result = APP_RPC_ERROR;

    RpcTryExcept
    {
        result = ActivateProduct(
            g_binding,
            activationCode
        );
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        result = APP_RPC_ERROR;
        InvalidateBindingLocked();
    }
    RpcEndExcept

        return result;
}

static long CallGetAntivirusStatus(boolean* running)
{
    long result = APP_RPC_ERROR;

    RpcTryExcept
    {
        result = GetAntivirusStatus(g_binding, running);
    }
        RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        result = APP_RPC_ERROR;
        InvalidateBindingLocked();
    }
    RpcEndExcept

    return result;
}


// ------------------------------------------------------------
// Public client API
// ------------------------------------------------------------

static bool CallStopService()
{
    boolean stopped = FALSE;

    RpcTryExcept
    {
        stopped = StopService(g_binding);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        stopped = FALSE;
        InvalidateBindingLocked();
    }
    RpcEndExcept

    return stopped != FALSE;
}

bool RpcStopService()
{
    std::lock_guard<std::mutex> lock(g_bindingMutex);
    if (!EnsureBindingLocked()) return false;
    return CallStopService();
}

ClientAuthState RpcGetCurrentUser(long* result)
{
    ClientAuthState state;

    std::lock_guard<std::mutex> lock(g_bindingMutex);
    if (!EnsureBindingLocked())
    {
        if (result)
            *result = APP_RPC_ERROR;

        return state;
    }

    boolean authenticated = FALSE;
    wchar_t username[128] = {};

    long rpcResult = CallGetCurrentUser(
        &authenticated,
        128,
        username
    );

    if (result)
        *result = rpcResult;

    if (rpcResult == APP_OK)
    {
        state.authenticated = (authenticated != FALSE);
        state.username = username;
    }

    return state;
}


long RpcLogin(
    const std::wstring& username,
    const std::wstring& password)
{
    std::lock_guard<std::mutex> lock(g_bindingMutex);
    if (!EnsureBindingLocked())
        return APP_RPC_ERROR;

    return CallLogin(
        username.c_str(),
        password.c_str()
    );
}


long RpcLogout()
{
    std::lock_guard<std::mutex> lock(g_bindingMutex);
    if (!EnsureBindingLocked())
        return APP_RPC_ERROR;

    return CallLogout();
}


ClientLicenseState RpcGetLicenseInfo(long* result)
{
    ClientLicenseState state;

    std::lock_guard<std::mutex> lock(g_bindingMutex);
    if (!EnsureBindingLocked())
    {
        if (result)
            *result = APP_RPC_ERROR;

        return state;
    }

    boolean licensed = FALSE;
    __int64 expiresUnix = 0;

    long rpcResult = CallGetLicenseInfo(
        &licensed,
        &expiresUnix
    );

    if (result)
        *result = rpcResult;

    state.licensed = (licensed != FALSE);
    state.expiresUnix = expiresUnix;

    return state;
}

bool RpcGetAntivirusStatus(long* result)
{
    std::lock_guard<std::mutex> lock(g_bindingMutex);
    if (!EnsureBindingLocked()) {
        if (result) *result = APP_RPC_ERROR;
        return false;
    }

    boolean running = FALSE;
    const long rpcResult = CallGetAntivirusStatus(&running);
    if (result) *result = rpcResult;
    return rpcResult == APP_OK && running != FALSE;
}


long RpcActivateProduct(const std::wstring& code)
{
    std::lock_guard<std::mutex> lock(g_bindingMutex);
    if (!EnsureBindingLocked())
        return APP_RPC_ERROR;

    return CallActivateProduct(
        code.c_str()
    );
}


// ------------------------------------------------------------
// MIDL memory management
// ------------------------------------------------------------

extern "C" void* __RPC_USER MIDL_user_allocate(size_t size)
{
    return std::malloc(size);
}


extern "C" void __RPC_USER MIDL_user_free(void* ptr)
{
    std::free(ptr);
}
