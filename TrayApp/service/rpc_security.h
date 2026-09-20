#pragma once

#include <windows.h>
#include <rpc.h>
#include <string>

namespace RpcSecurity {

struct CallerIdentity {
    std::wstring sid;
    DWORD sessionId = 0;
};

bool GetCallerIdentity(handle_t binding, CallerIdentity& identity);

}
