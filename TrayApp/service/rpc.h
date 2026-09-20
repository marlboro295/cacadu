#pragma once
#include <windows.h>
bool StartRpcServer(HANDLE hStopEvent, HANDLE hReadyEvent);
bool IsRpcServerReady();
void StopRpcServer();
