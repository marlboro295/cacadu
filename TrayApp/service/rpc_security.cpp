#include "rpc_security.h"

#include <sddl.h>
#include <vector>

namespace RpcSecurity {

namespace {

class ScopedImpersonation {
public:
    explicit ScopedImpersonation(handle_t binding)
        : active_(RpcImpersonateClient(binding) == RPC_S_OK) {}

    ScopedImpersonation(const ScopedImpersonation&) = delete;
    ScopedImpersonation& operator=(const ScopedImpersonation&) = delete;

    bool Active() const { return active_; }

    bool Revert() {
        if (!active_) return true;
        const RPC_STATUS status = RpcRevertToSelf();
        if (status == RPC_S_OK) {
            active_ = false;
            return true;
        }
        OutputDebugStringW(L"TrayService: RpcRevertToSelf failed\n");
        return false;
    }

    ~ScopedImpersonation() {
        if (active_ && RpcRevertToSelf() != RPC_S_OK)
            OutputDebugStringW(
                L"TrayService: RpcRevertToSelf cleanup failed\n");
    }

private:
    bool active_ = false;
};

}

bool GetCallerIdentity(handle_t binding, CallerIdentity& identity) {
    identity = {};
    if (!binding) return false;

    RPC_AUTHZ_HANDLE privileges = nullptr;
    RPC_WSTR principalName = nullptr;
    unsigned long authnLevel = RPC_C_AUTHN_LEVEL_NONE;
    unsigned long authnService = RPC_C_AUTHN_NONE;
    unsigned long authzService = RPC_C_AUTHZ_NONE;

    RPC_STATUS status = RpcBindingInqAuthClient(
        binding,
        &privileges,
        &principalName,
        &authnLevel,
        &authnService,
        &authzService);

    if (principalName && RpcStringFreeW(&principalName) != RPC_S_OK)
        OutputDebugStringW(L"TrayService: RpcStringFreeW failed\n");

    if (status != RPC_S_OK ||
        authnService == RPC_C_AUTHN_NONE ||
        authnLevel < RPC_C_AUTHN_LEVEL_PKT_INTEGRITY) {
        return false;
    }

    ScopedImpersonation impersonation(binding);
    if (!impersonation.Active())
        return false;

    bool success = false;
    HANDLE token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        DWORD userSize = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &userSize);
        if (userSize != 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            std::vector<BYTE> userBuffer(userSize);
            if (GetTokenInformation(token, TokenUser,
                                    userBuffer.data(), userSize, &userSize)) {
                const TOKEN_USER* user =
                    reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
                LPWSTR sidString = nullptr;
                if (ConvertSidToStringSidW(user->User.Sid, &sidString)) {
                    DWORD sessionId = 0;
                    DWORD sessionSize = sizeof(sessionId);
                    if (GetTokenInformation(token, TokenSessionId,
                                            &sessionId, sessionSize,
                                            &sessionSize)) {
                        identity.sid = sidString;
                        identity.sessionId = sessionId;
                        success = true;
                    }
                    LocalFree(sidString);
                }
            }
        }
        CloseHandle(token);
    }

    // Do not return a caller-derived identity if the thread could not be
    // restored to the service token.
    const bool reverted = impersonation.Revert();
    return success && reverted;
}

}
