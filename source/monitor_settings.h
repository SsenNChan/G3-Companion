#pragma once
#include "manager_handoff.h"
#include "startup.h"
#include "localization.h"

namespace Monitoring {
inline constexpr wchar_t StartupFilename[]=L"G3 Companion 0.0.7-alpha.lnk";
struct Paths { std::wstring executable,directory,settings,startup; };
struct State { bool enabled=false,startup=false,lowBatteryNotifications=true; HRESULT error=S_OK; };

inline HRESULT CurrentPaths(Paths& paths) {
    wchar_t module[32768]{};
    DWORD length=GetModuleFileNameW(nullptr,module,ARRAYSIZE(module));
    if(!length)return HRESULT_FROM_WIN32(GetLastError());
    if(length>=ARRAYSIZE(module))return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    paths.executable.assign(module,length);
    paths.directory=paths.executable.substr(0,paths.executable.find_last_of(L"\\/"));
    paths.settings=paths.directory+L"\\settings-0.0.7-alpha.ini";
    PWSTR folder=nullptr;
    HRESULT result=SHGetKnownFolderPath(FOLDERID_Startup,0,nullptr,&folder);
    if(FAILED(result))return result;
    paths.startup=std::wstring(folder)+L"\\"+StartupFilename;
    CoTaskMemFree(folder);
    return S_OK;
}
inline HRESULT StoreEnabled(const Paths& paths,bool enabled) {
    if(WritePrivateProfileStringW(L"Monitoring",L"TrayEnabled",enabled?L"1":L"0",paths.settings.c_str()))return S_OK;
    return HRESULT_FROM_WIN32(GetLastError());
}
inline State Read(const Paths& paths) {
    State state;
    state.enabled=GetPrivateProfileIntW(L"Monitoring",L"TrayEnabled",0,paths.settings.c_str())==1;
    state.lowBatteryNotifications=GetPrivateProfileIntW(L"Monitoring",L"LowBatteryNotificationsEnabled",1,paths.settings.c_str())==1;
    auto startup=Startup::Inspect(paths.startup,paths.executable,paths.directory,L"--tray");
    state.startup=state.enabled&&(startup==Startup::State::Enabled||startup==Startup::State::RepairNeeded);
    if(startup==Startup::State::Error)state.error=E_FAIL;
    return state;
}
inline HRESULT CancelStartup(const Paths& paths) {
    if(DeleteFileW(paths.startup.c_str()))return S_OK;
    DWORD error=GetLastError();
    return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?S_OK:HRESULT_FROM_WIN32(error);
}
inline HRESULT SynchronizeStartup(const Paths& paths) {
    auto state=Read(paths);
    if(FAILED(state.error))return state.error;
    if(!state.enabled)return CancelStartup(paths);
    if(state.startup)return Startup::Repair(paths.startup,paths.executable,paths.directory,L"--tray");
    return S_OK;
}
inline HANDLE OpenTray(const Paths& paths,HWND& contact) {
    contact=FindWindowW(Handoff::TrayClass,nullptr);
    if(!contact)return nullptr;
    DWORD pid=0;GetWindowThreadProcessId(contact,&pid);
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
    if(!Handoff::ProcessMatches(process,paths.directory,Handoff::TrayFilename)){
        if(process)CloseHandle(process);
        contact=nullptr;SetLastError(ERROR_REVISION_MISMATCH);return nullptr;
    }
    return process;
}
inline HRESULT EnsureTray(const Paths& paths) {
    HWND contact=nullptr;
    if(HANDLE process=OpenTray(paths,contact)){CloseHandle(process);return S_OK;}
    if(FindWindowW(Handoff::TrayClass,nullptr))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    std::wstring command=L"\""+paths.executable+L"\" --tray"+Localization::LaunchArgument();
    STARTUPINFOW start{sizeof(start)};PROCESS_INFORMATION process{};
    if(!CreateProcessW(paths.executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,paths.directory.c_str(),&start,&process))
        return HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(process.hThread);
    // The tray creates its contact window before acquiring any HID handles.
    DWORD input=WaitForInputIdle(process.hProcess,5000);
    bool alive=WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT;
    CloseHandle(process.hProcess);
    if(HANDLE existing=OpenTray(paths,contact)){CloseHandle(existing);return S_OK;}
    if(!alive)return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
    return HRESULT_FROM_WIN32(input==WAIT_TIMEOUT?ERROR_TIMEOUT:ERROR_PROCESS_ABORTED);
}
inline HRESULT StopTray(const Paths& paths) {
    HWND contact=nullptr;
    HANDLE process=OpenTray(paths,contact);
    if(!process)return S_OK;
    DWORD_PTR acknowledged=0;
    LRESULT delivered=SendMessageTimeoutW(contact,Handoff::StopTrayMessage(),0,0,SMTO_ABORTIFHUNG,5000,&acknowledged);
    HRESULT result=S_OK;
    if(!delivered||acknowledged!=1)result=HRESULT_FROM_WIN32(delivered?ERROR_BUSY:GetLastError());
    else if(WaitForSingleObject(process,5000)!=WAIT_OBJECT_0)result=HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    CloseHandle(process);
    return result;
}
inline HRESULT SetEnabled(const Paths& paths,bool enabled) {
    if(enabled){
        HRESULT result=StoreEnabled(paths,true);
        if(FAILED(result))return result;
        result=EnsureTray(paths);
        if(FAILED(result))StoreEnabled(paths,false);
        return result;
    }
    HRESULT result=CancelStartup(paths);
    if(FAILED(result))return result;
    // Save first so a tray that is still starting will also observe the disabled setting.
    result=StoreEnabled(paths,false);
    if(FAILED(result))return result;
    result=StopTray(paths);
    if(FAILED(result)){
        HWND contact=nullptr;
        if(HANDLE process=OpenTray(paths,contact)){CloseHandle(process);StoreEnabled(paths,true);}
    }
    return result;
}
inline HRESULT SetStartup(const Paths& paths,bool enabled) {
    if(enabled && !Read(paths).enabled)return E_INVALIDARG;
    return enabled?Startup::Repair(paths.startup,paths.executable,paths.directory,L"--tray"):CancelStartup(paths);
}
inline HRESULT SetLowBatteryNotifications(const Paths& paths,bool enabled) {
    if(!WritePrivateProfileStringW(L"Monitoring",L"LowBatteryNotificationsEnabled",enabled?L"1":L"0",paths.settings.c_str()))
        return HRESULT_FROM_WIN32(GetLastError());
    HWND contact=nullptr;
    if(HANDLE process=OpenTray(paths,contact)){
        PostMessageW(contact,Handoff::NotificationsChangedMessage(),0,0);
        CloseHandle(process);
    }
    return S_OK;
}
inline HRESULT TestNotification(const Paths& paths) {
    if(!Read(paths).enabled)return E_INVALIDARG;
    HRESULT result=EnsureTray(paths);
    if(FAILED(result))return result;
    HWND contact=nullptr;HANDLE process=OpenTray(paths,contact);
    if(!process)return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
    DWORD_PTR acknowledged=0;
    SetLastError(ERROR_SUCCESS);
    LRESULT delivered=SendMessageTimeoutW(contact,Handoff::TestNotificationMessage(),0,0,SMTO_ABORTIFHUNG,3000,&acknowledged);
    DWORD error=GetLastError();CloseHandle(process);
    if(!delivered)return HRESULT_FROM_WIN32(error?error:ERROR_TIMEOUT);
    return acknowledged==1?S_OK:E_FAIL;
}
}
