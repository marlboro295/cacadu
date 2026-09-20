#include <windows.h>
#include <string>
#include <ctime>
#include "resource.h"
#include "TrayIcon.h"
#include "process_utils.h"
#include "rpc_client.h"
#include "service_control.h"

static const wchar_t* WINDOW_CLASS=L"TrayAppWindowClass";
static HWND eLogin,ePassword,bLogin,bLogout,eCode,bActivate,lStatus,lUser,lLicense;
static HANDLE g_serviceProcess=nullptr;
static bool g_monitorServiceViaScm = false;

static void CloseServiceProcessHandle(){
    if(g_serviceProcess){CloseHandle(g_serviceProcess);g_serviceProcess=nullptr;}
}

static HMENU CreateMainMenu(){
    HMENU bar=CreateMenu();
    if(!bar) return nullptr;
    HMENU file=CreatePopupMenu();
    if(!file){DestroyMenu(bar);return nullptr;}
    if(!AppendMenuW(file,MF_STRING,ID_FILE_EXIT,L"Выход") ||
       !AppendMenuW(bar,MF_POPUP,(UINT_PTR)file,L"Файл")){
        DestroyMenu(file);DestroyMenu(bar);return nullptr;
    }
    return bar;
}
static void Show(HWND h,bool v){ShowWindow(h,v?SW_SHOW:SW_HIDE);}
static std::wstring DateText(long long unixTime){
    if(unixTime<=0) return L"без указанного срока";
    time_t t=(time_t)unixTime; tm local{};
    localtime_s(&local,&t); wchar_t b[64]{};
    wcsftime(b,64,L"%d.%m.%Y %H:%M",&local); return b;
}
static std::wstring ResultText(long r){
    switch(r){
    case APP_UNAUTHORIZED:return L"Неверный логин или пароль.";
    case APP_HTTP_ERROR:return L"Сервер недоступен или вернул ошибку.";
    case APP_BAD_RESPONSE:return L"Некорректный ответ сервера.";
    case APP_NO_LICENSE:return L"Активная лицензия отсутствует.";
    case APP_ACTIVATION_FAILED:return L"Код активации отклонён.";
    case APP_RPC_ERROR:return L"Windows-служба недоступна.";
    default:return L"Операция завершилась с ошибкой.";
    }
}

static void ShowStartupError(const std::wstring& message,DWORD error=ERROR_SUCCESS){
    std::wstring text=message;
    if(error!=ERROR_SUCCESS) text+=L"\r\nКод Win32: "+std::to_wstring(error);
    MessageBoxW(nullptr,text.c_str(),L"TrayApp",MB_OK|MB_ICONERROR);
}
static bool RequestServiceStop(HWND hWnd){
    if(RpcStopService()){
        DestroyWindow(hWnd);
        return true;
    }
    if(lStatus) SetWindowTextW(lStatus,L"Не удалось остановить Windows-службу.");
    MessageBoxW(hWnd,L"Не удалось остановить Windows-службу.",
                L"TrayApp",MB_OK|MB_ICONERROR);
    return false;
}

static void RefreshUi(){
    long ar=0; auto a=RpcGetCurrentUser(&ar);
    if(ar!=APP_OK){
        SetWindowTextW(lUser,L"Пользователь: состояние недоступно");
        SetWindowTextW(lLicense,L"Состояние лицензии недоступно");
        SetWindowTextW(lStatus,ResultText(ar).c_str());
        Show(eLogin,true);Show(ePassword,true);Show(bLogin,true);
        Show(bLogout,false);Show(eCode,false);Show(bActivate,false);
        return;
    }
    if(!a.authenticated){
        SetWindowTextW(lUser,L"Пользователь: не выполнен вход");
        SetWindowTextW(lLicense,L"Антивирус: заблокирован");
        Show(eLogin,true);Show(ePassword,true);Show(bLogin,true);
        Show(bLogout,false);Show(eCode,false);Show(bActivate,false);
        return;
    }
    std::wstring user=L"Пользователь: "+a.username;
    SetWindowTextW(lUser,user.c_str());
    Show(eLogin,false);Show(ePassword,false);Show(bLogin,false);Show(bLogout,true);

    long lr=0; auto lic=RpcGetLicenseInfo(&lr);
    if(lr!=APP_OK && lr!=APP_NO_LICENSE){
        SetWindowTextW(lLicense,L"Состояние лицензии недоступно");
        SetWindowTextW(lStatus,ResultText(lr).c_str());
        Show(eCode,false);Show(bActivate,false);
        return;
    }
    long avResult=0;
    const bool antivirusRunning=RpcGetAntivirusStatus(&avResult);
    if(lic.licensed){
        std::wstring s=L"Лицензия активна до: "+DateText(lic.expiresUnix)+
            (avResult==APP_OK && antivirusRunning
                ? L"\r\nАнтивирус: доступен"
                : L"\r\nАнтивирус: заблокирован");
        SetWindowTextW(lLicense,s.c_str());
        SetWindowTextW(lStatus,avResult==APP_OK ? L"" : ResultText(avResult).c_str());
        Show(eCode,false);Show(bActivate,false);
    } else {
        SetWindowTextW(lLicense,L"Лицензия отсутствует\r\nАнтивирус: заблокирован");
        SetWindowTextW(lStatus,L"");
        Show(eCode,true);Show(bActivate,true);
    }
}
static HWND C(HWND p,const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id){
    return CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,p,(HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr),nullptr);
}
LRESULT CALLBACK WndProc(HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam){
    UINT tb=GetTaskbarCreatedMessage();
    if(tb && msg==tb){
        if(!AddTrayIcon(hWnd)) OutputDebugStringW(L"TrayApp: unable to restore tray icon\n");
        return 0;
    }
    switch(msg){
    case WM_CREATE:
        {
        HMENU menu=CreateMainMenu();
        if(!menu) return -1;
        SetMenu(hWnd,menu);
        C(hWnd,L"STATIC",L"TrayApp Security",SS_LEFT,30,25,350,28,0);
        lUser=C(hWnd,L"STATIC",L"",SS_LEFT,30,65,520,24,ID_USER);
        lLicense=C(hWnd,L"STATIC",L"",SS_LEFT,30,95,520,45,ID_LICENSE);
        eLogin=C(hWnd,L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,30,155,250,28,ID_EDIT_LOGIN);
        ePassword=C(hWnd,L"EDIT",L"",WS_BORDER|ES_PASSWORD|ES_AUTOHSCROLL,30,193,250,28,ID_EDIT_PASSWORD);
        bLogin=C(hWnd,L"BUTTON",L"Войти",BS_PUSHBUTTON,300,155,130,66,ID_BTN_LOGIN);
        bLogout=C(hWnd,L"BUTTON",L"Выйти",BS_PUSHBUTTON,440,65,110,28,ID_BTN_LOGOUT);
        eCode=C(hWnd,L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,30,155,250,28,ID_EDIT_ACTIVATION);
        bActivate=C(hWnd,L"BUTTON",L"Активировать",BS_PUSHBUTTON,300,155,130,28,ID_BTN_ACTIVATE);
        lStatus=C(hWnd,L"STATIC",L"",SS_LEFT,30,245,520,55,ID_STATUS);
        if(!SetTimer(hWnd,ID_TIMER_REFRESH,5000,nullptr))
            OutputDebugStringW(L"TrayApp: unable to create refresh timer\n");
        RefreshUi(); return 0;
        }
    case WM_TIMER:
        if(wParam==ID_TIMER_REFRESH){
            if(g_serviceProcess){
                const DWORD waitResult=WaitForSingleObject(g_serviceProcess,0);
                if(waitResult==WAIT_OBJECT_0 || waitResult==WAIT_FAILED){
                    CloseHandle(g_serviceProcess);g_serviceProcess=nullptr;
                    DestroyWindow(hWnd);
                    return 0;
                }
            }
            else if (g_monitorServiceViaScm) {
                TrayServiceStatus serviceStatus{};
                if (!QueryTrayServiceStatus(serviceStatus) ||
                    serviceStatus.state == SERVICE_STOPPED) {
                    DestroyWindow(hWnd);
                    return 0;
                }
            }
            RefreshUi();
        }
        return 0;
    case WM_TRAYICON:
        if(lParam==WM_LBUTTONUP)ShowMainWindow(hWnd);
        else if(lParam==WM_RBUTTONUP){
            if(!ShowContextMenu(hWnd))
                MessageBoxW(hWnd,L"Не удалось открыть меню уведомлений.",L"TrayApp",MB_OK|MB_ICONERROR);
        }
        return 0;
    case WM_COMMAND:
        switch(LOWORD(wParam)){
        case ID_BTN_LOGIN:{
            wchar_t u[256]{},p[256]{};GetWindowTextW(eLogin,u,256);GetWindowTextW(ePassword,p,256);
            SetWindowTextW(ePassword,L"");
            long r=RpcLogin(u,p); SecureZeroMemory(p,sizeof(p));
            if(r!=APP_OK)SetWindowTextW(lStatus,ResultText(r).c_str()); else RefreshUi(); return 0;}
        case ID_BTN_LOGOUT:{
            long r=RpcLogout();
            if(r!=APP_OK) SetWindowTextW(lStatus,ResultText(r).c_str());
            else {SetWindowTextW(ePassword,L"");RefreshUi();}
            return 0;
        }
        case ID_BTN_ACTIVATE:{
            wchar_t c[256]{};GetWindowTextW(eCode,c,256);long r=RpcActivateProduct(c);
            SecureZeroMemory(c,sizeof(c));
            if(r!=APP_OK)SetWindowTextW(lStatus,ResultText(r).c_str());else RefreshUi();return 0;}
        case ID_TRAY_OPEN:ShowMainWindow(hWnd);return 0;
        case ID_TRAY_EXIT:case ID_FILE_EXIT:
            RequestServiceStop(hWnd);
            return 0;
        } return 0;
    case WM_CLOSE:ShowWindow(hWnd,SW_HIDE);return 0;
    case WM_DESTROY:
        KillTimer(hWnd,ID_TIMER_REFRESH);
        RpcDisconnect();
        RemoveTrayIcon(hWnd);
        if(g_serviceProcess){CloseHandle(g_serviceProcess);g_serviceProcess=nullptr;}
        g_monitorServiceViaScm = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}
int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    LaunchOptions options=ParseLaunchOptions();
    if(!options.valid){
        ShowStartupError(L"Некорректные аргументы командной строки.");
        return 1;
    }

    if(options.hasParentPid){
        std::wstring parentError;
        if(!OpenValidatedServiceParent(options,GetCurrentProcessId(),
                                       g_serviceProcess,parentError)){
            ShowStartupError(parentError);
            return 1;
        }
    }else{
        g_monitorServiceViaScm = true;
        TrayServiceStatus serviceStatus{};
        if(!QueryTrayServiceStatus(serviceStatus)){
            ShowStartupError(L"Не удалось получить состояние TrayService.",GetLastError());
            return 1;
        }
        if(serviceStatus.state==SERVICE_RUNNING){
            return 0;
        }
        if(serviceStatus.state==SERVICE_STOP_PENDING){
            ShowStartupError(L"TrayService находится в процессе остановки.");
            return 1;
        }
        DWORD error=ERROR_SUCCESS;
        if(!StartTrayServiceAndWait(30000,&error)){
            ShowStartupError(L"Не удалось запустить TrayService.",error);
            return 1;
        }
        return 0;
    }

    std::wstring sid;
    if(!GetCurrentUserSidString(sid)){
        ShowStartupError(L"Не удалось определить SID текущего пользователя.",GetLastError());
        CloseServiceProcessHandle();
        return 1;
    }
    std::wstring mutexName=L"Global\\TrayApp_SingleInstance_"+sid;
    SetLastError(ERROR_SUCCESS);
    HANDLE mutex=CreateMutexW(nullptr,TRUE,mutexName.c_str());
    DWORD mutexError=GetLastError();
    if(!mutex){
        ShowStartupError(L"Не удалось создать mutex экземпляра.",mutexError);
        CloseServiceProcessHandle();
        return 1;
    }
    if(mutexError==ERROR_ALREADY_EXISTS){CloseHandle(mutex);CloseServiceProcessHandle();return 0;}
    if(!InitTrayMessages()){
        ShowStartupError(L"Не удалось зарегистрировать сообщение TaskbarCreated.",GetLastError());
        CloseHandle(mutex);CloseServiceProcessHandle();return 1;
    }
    WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.lpfnWndProc=WndProc;wc.hInstance=hInstance;
    wc.lpszClassName=WINDOW_CLASS;wc.hIcon=LoadIconW(hInstance,MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm=wc.hIcon;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    if(!RegisterClassExW(&wc)){CloseHandle(mutex);CloseServiceProcessHandle();return 1;}
    HWND h=CreateWindowExW(0,WINDOW_CLASS,L"TrayApp",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,620,380,nullptr,nullptr,hInstance,nullptr);
    if(!h){CloseHandle(mutex);CloseServiceProcessHandle();return 1;}
    if(!AddTrayIcon(h)){
        ShowStartupError(L"Не удалось добавить значок в область уведомлений.",GetLastError());
        DestroyWindow(h);CloseHandle(mutex);CloseServiceProcessHandle();return 1;
    }
    if(!options.silent&&nCmdShow!=SW_HIDE){ShowWindow(h,nCmdShow);UpdateWindow(h);}
    MSG m{};while(GetMessageW(&m,nullptr,0,0)){TranslateMessage(&m);DispatchMessageW(&m);}
    CloseHandle(mutex);return (int)m.wParam;
}
