#include "TrayIcon.h"
#include "resource.h"
#include <shellapi.h>

static UINT g_WM_TASKBARCREATED = 0;

bool InitTrayMessages() {
    g_WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    return g_WM_TASKBARCREATED != 0;
}

UINT GetTaskbarCreatedMessage() {
    return g_WM_TASKBARCREATED;
}

static bool FillNid(NOTIFYICONDATAW& nid, HWND hWnd) {
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hWnd;
    nid.uID    = IDI_APP_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon  = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!nid.hIcon) return false;
    wcscpy_s(nid.szTip, L"TrayApp");
    return true;
}

bool AddTrayIcon(HWND hWnd) {
    NOTIFYICONDATAW nid;
    if (!FillNid(nid, hWnd)) return false;
    return Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;
}

void RemoveTrayIcon(HWND hWnd) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hWnd;
    nid.uID    = IDI_APP_ICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

bool ShowContextMenu(HWND hWnd) {
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return false;

    bool ok = AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, L"Открыть") != FALSE;
    ok = AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Выход") != FALSE && ok;
    if (!ok) {
        DestroyMenu(hMenu);
        return false;
    }

    // ОБЯЗАТЕЛЬНО, иначе меню может не реагировать на клики
    SetForegroundWindow(hWnd);

    bool shown = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL) != FALSE;
    DestroyMenu(hMenu);
    return shown;
}

void ShowMainWindow(HWND hWnd) {
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);
    SetForegroundWindow(hWnd);
}
