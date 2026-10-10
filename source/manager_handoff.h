#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <cstdint>

// Both applications keep this mutex for the lifetime of their HID access.
// The tray releases its handles before acknowledging the registered message.
namespace Handoff {
inline constexpr wchar_t TrayClass[]=L"PeripheralMonitor.Window";
inline constexpr wchar_t ManagerClass[]=L"MCHOSE.Legacy.Manager.Window";
inline constexpr wchar_t DisplayWindowProperty[]=L"MCHOSE.Legacy.Manager.DisplayWindow";
inline constexpr wchar_t TrayFilename[]=L"G3-Companion.exe";
inline constexpr wchar_t ManagerFilename[]=L"G3-Companion.exe";
inline UINT StopTrayMessage(){static UINT value=RegisterWindowMessageW(L"MCHOSE.Legacy.Tray.Stop.Alpha.0.0.7-alpha");return value;}
inline UINT TrayStateMessage(){static UINT value=RegisterWindowMessageW(L"MCHOSE.Legacy.Tray.State.Alpha.0.0.7-alpha");return value;}
inline UINT NotificationsChangedMessage(){static UINT value=RegisterWindowMessageW(L"MCHOSE.Legacy.Tray.NotificationsChanged.Alpha.0.0.7-alpha");return value;}
inline UINT TestNotificationMessage(){static UINT value=RegisterWindowMessageW(L"MCHOSE.Legacy.Tray.TestNotification.Alpha.0.0.7-alpha");return value;}
inline UINT RequestMessage(){static UINT value=RegisterWindowMessageW(L"MCHOSE.Legacy.DeviceHandoff.Request.Alpha.0.0.7-alpha");return value;}
inline std::wstring NormalizePath(const std::wstring& value){
    wchar_t full[32768]{};DWORD count=GetFullPathNameW(value.c_str(),ARRAYSIZE(full),full,nullptr);
    std::wstring path=count&&count<ARRAYSIZE(full)?std::wstring(full,count):value;
    for(auto& c:path)if(c==L'/')c=L'\\';
    while(path.size()>3&&path.back()==L'\\')path.pop_back();
    CharLowerBuffW(path.data(),static_cast<DWORD>(path.size()));return path;
}
inline std::wstring MutexName(const std::wstring& executableDirectory){
    std::uint64_t hash=14695981039346656037ull;
    for(wchar_t c:NormalizePath(executableDirectory)){hash^=static_cast<std::uint16_t>(c);hash*=1099511628211ull;}
    return L"Local\\MCHOSE.Legacy.DeviceOwner.Alpha.0.0.7-alpha."+std::to_wstring(hash);
}
inline bool ProcessMatches(HANDLE process,const std::wstring& executableDirectory,const wchar_t* filename){
    wchar_t path[32768]{};DWORD count=ARRAYSIZE(path);
    return process&&QueryFullProcessImageNameW(process,0,path,&count)&&
        NormalizePath(std::wstring(path,count))==NormalizePath(executableDirectory+L"\\"+filename);
}
inline HANDLE OpenManagerProcess(HWND managerWindow,const std::wstring& executableDirectory,DWORD& pid){
    pid=0;if(!IsWindow(managerWindow))return nullptr;
    wchar_t name[128]{};GetClassNameW(managerWindow,name,ARRAYSIZE(name));
    if(wcscmp(name,ManagerClass)!=0)return nullptr;
    GetWindowThreadProcessId(managerWindow,&pid);
    HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!ProcessMatches(process,executableDirectory,ManagerFilename)||WaitForSingleObject(process,0)!=WAIT_TIMEOUT){if(process)CloseHandle(process);return nullptr;}
    return process;
}
inline HWND FindManager(const std::wstring& executableDirectory){
    HWND candidate=nullptr;
    while((candidate=FindWindowExW(nullptr,candidate,ManagerClass,nullptr))!=nullptr){
        DWORD pid=0;HANDLE process=OpenManagerProcess(candidate,executableDirectory,pid);
        if(process){CloseHandle(process);return candidate;}
    }
    return nullptr;
}
inline void ActivateManager(HWND managerWindow){
    if(!IsWindow(managerWindow))return;DWORD pid=0;GetWindowThreadProcessId(managerWindow,&pid);
    HWND display=reinterpret_cast<HWND>(GetPropW(managerWindow,DisplayWindowProperty));DWORD displayPid=0;
    if(display&&IsWindow(display))GetWindowThreadProcessId(display,&displayPid);
    if(!display||displayPid!=pid)display=managerWindow;
    AllowSetForegroundWindow(pid);ShowWindow(display,IsIconic(display)?SW_RESTORE:SW_SHOW);SetForegroundWindow(display);
}
inline HANDLE managerMutex=nullptr;
inline bool managerOwnsDevice=false;
inline void ReleaseForManager(){
    if(managerMutex){if(managerOwnsDevice)ReleaseMutex(managerMutex);CloseHandle(managerMutex);managerMutex=nullptr;managerOwnsDevice=false;}
}
inline bool AcquireForManager(HWND managerWindow,const std::wstring& executableDirectory){
    if(managerOwnsDevice)return true;
    if(!IsWindow(managerWindow)){SetLastError(ERROR_INVALID_WINDOW_HANDLE);return false;}
    DWORD managerPid=0;GetWindowThreadProcessId(managerWindow,&managerPid);
    if(managerPid!=GetCurrentProcessId()){SetLastError(ERROR_ACCESS_DENIED);return false;}
    managerMutex=CreateMutexW(nullptr,FALSE,MutexName(executableDirectory).c_str());
    if(!managerMutex)return false;
    auto askTray=[&](){
        HWND tray=FindWindowW(TrayClass,nullptr);if(!tray)return true;
        DWORD pid=0;GetWindowThreadProcessId(tray,&pid);HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        bool compatible=ProcessMatches(process,executableDirectory,TrayFilename);if(process)CloseHandle(process);
        if(!compatible){SetLastError(ERROR_REVISION_MISMATCH);return false;}
        DWORD_PTR acknowledged=0;
        if(!SendMessageTimeoutW(tray,RequestMessage(),reinterpret_cast<WPARAM>(managerWindow),managerPid,SMTO_ABORTIFHUNG|SMTO_BLOCK,8000,&acknowledged))return false;
        if(acknowledged!=1){SetLastError(ERROR_BUSY);return false;}return true;
    };
    if(!askTray()){DWORD error=GetLastError();ReleaseForManager();SetLastError(error);return false;}
    DWORD acquired=WaitForSingleObject(managerMutex,0);
    if(acquired==WAIT_TIMEOUT){
        if(!askTray()){DWORD error=GetLastError();ReleaseForManager();SetLastError(error);return false;}
        acquired=WaitForSingleObject(managerMutex,5000);
    }
    if(acquired==WAIT_OBJECT_0||acquired==WAIT_ABANDONED){managerOwnsDevice=true;return true;}
    DWORD error=acquired==WAIT_TIMEOUT?ERROR_BUSY:GetLastError();ReleaseForManager();SetLastError(error);return false;
}
}
