#pragma once
#include <string>

enum AppResult {
    APP_OK=0, APP_UNAUTHORIZED=1, APP_HTTP_ERROR=2, APP_BAD_RESPONSE=3,
    APP_NO_LICENSE=4, APP_ACTIVATION_FAILED=5, APP_RPC_ERROR=100
};
struct ClientAuthState { bool authenticated=false; std::wstring username; };
struct ClientLicenseState { bool licensed=false; long long expiresUnix=0; };

bool RpcConnect();
void RpcDisconnect();
bool RpcStopService();
ClientAuthState RpcGetCurrentUser(long* result=nullptr);
long RpcLogin(const std::wstring& username,const std::wstring& password);
long RpcLogout();
ClientLicenseState RpcGetLicenseInfo(long* result=nullptr);
bool RpcGetAntivirusStatus(long* result=nullptr);
long RpcActivateProduct(const std::wstring& code);
