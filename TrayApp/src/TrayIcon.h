#pragma once
#include <windows.h>

bool InitTrayMessages();
UINT GetTaskbarCreatedMessage();
bool AddTrayIcon(HWND hWnd);
void RemoveTrayIcon(HWND hWnd);
bool ShowContextMenu(HWND hWnd);
void ShowMainWindow(HWND hWnd);
