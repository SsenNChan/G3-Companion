#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbt.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include "../manager_backend.h"
#include "../manager_handoff.h"

// Start, Apply, Refresh and Stop are called on the window's UI thread.
// HID work stays in ManagerBackend; this window only transfers completed snapshots.
class DeviceController {
public:
    using Settings=ManagerBackend::Settings;
    using Snapshot=ManagerBackend::Snapshot;
    using Callback=std::function<void(const Snapshot&)>;

    DeviceController()=default;
    DeviceController(const DeviceController&)=delete;
    DeviceController& operator=(const DeviceController&)=delete;
    ~DeviceController(){Stop();}

    bool Start(HWND actualDisplayWindow,Callback callback) {
        if(started_){SetLastError(ERROR_ALREADY_INITIALIZED);return false;}
        if(!IsWindow(actualDisplayWindow)){SetLastError(ERROR_INVALID_WINDOW_HANDLE);return false;}
        callback_=std::move(callback);displayWindow_=actualDisplayWindow;
        if(!GetDirectory())return Fail(GetLastError());

        singleton_=CreateMutexW(nullptr,FALSE,L"Local\\MCHOSE.Legacy.Manager.Singleton");
        if(!singleton_)return Fail(GetLastError());
        if(GetLastError()==ERROR_ALREADY_EXISTS){
            HWND existing=Handoff::FindManager(directory_);
            if(existing)Handoff::ActivateManager(existing);
            return Fail(ERROR_ALREADY_EXISTS);
        }

        const wchar_t* className=Handoff::ManagerClass;
        WNDCLASSEXW type{};type.cbSize=sizeof(type);type.hInstance=GetModuleHandleW(nullptr);
        type.lpfnWndProc=WindowProcedure;type.lpszClassName=className;
        if(!RegisterClassExW(&type)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return Fail(GetLastError());
        contact_=CreateWindowExW(0,className,L"G3 Companion",WS_OVERLAPPED,
            0,0,0,0,nullptr,nullptr,type.hInstance,this);
        if(!contact_)return Fail(GetLastError());
        if(!SetPropW(contact_,Handoff::DisplayWindowProperty,reinterpret_cast<HANDLE>(displayWindow_)))return Fail(GetLastError());


        Device::Result cached;LoadBatteryCache(cached);lastSavedSample_=cached.sampledAtUtc;
        const std::wstring logs=directory_+L"\\batteryhistory";
        if(!CreateDirectoryW(logs.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return Fail(GetLastError());
        if(!Handoff::AcquireForManager(contact_,directory_))return Fail(GetLastError());
        ownsDevice_=true;
        if(!ManagerBackend::Start(contact_,ResultMessage,logs,cached))return Fail(GetLastError());
        backendStarted_=true;started_=true;
        GUID guid;HidD_GetHidGuid(&guid);
        DEV_BROADCAST_DEVICEINTERFACE_W filter{sizeof(filter),DBT_DEVTYP_DEVICEINTERFACE,0,guid,{0}};
        deviceNotice_=RegisterDeviceNotificationW(contact_,&filter,DEVICE_NOTIFY_WINDOW_HANDLE);
        return true;
    }

    void Apply(const Settings& draft,const Settings& baseline,ManagerBackend::ApplyMode mode=ManagerBackend::ApplyMode::EditedSettings) {
        if(!started_)return;
        ManagerBackend::Apply(draft,baseline,mode);
    }

    void Refresh(bool topologyChanged=false) {
        if(!started_)return;
        ManagerBackend::Refresh(topologyChanged);
    }

    void Stop() {
        // Stop the producer before destroying its receiving window or draining messages.
        started_=false;
        if(backendStarted_){ManagerBackend::Stop();backendStarted_=false;}
        if(ownsDevice_){Handoff::ReleaseForManager();ownsDevice_=false;}
        if(deviceNotice_){UnregisterDeviceNotification(deviceNotice_);deviceNotice_=nullptr;}
        if(contact_){
            KillTimer(contact_,RefreshTimer);
            MSG pending{};
            while(PeekMessageW(&pending,contact_,ResultMessage,ResultMessage,PM_REMOVE))
                delete reinterpret_cast<Snapshot*>(pending.lParam);
            RemovePropW(contact_,Handoff::DisplayWindowProperty);
            DestroyWindow(contact_);contact_=nullptr;
        }
        if(singleton_){CloseHandle(singleton_);singleton_=nullptr;}
        callback_={};displayWindow_=nullptr;lastSavedSample_=0;
    }

private:
    static constexpr UINT ResultMessage=WM_APP+120;
    static constexpr UINT_PTR RefreshTimer=1;
    HWND contact_=nullptr,displayWindow_=nullptr;
    HANDLE singleton_=nullptr;
    HDEVNOTIFY deviceNotice_=nullptr;
    bool started_=false,backendStarted_=false,ownsDevice_=false;
    ULONGLONG lastSavedSample_=0;
    std::wstring directory_;
    Callback callback_;

    bool Fail(DWORD error){Stop();SetLastError(error);return false;}
    bool GetDirectory() {
        wchar_t path[32768]{};const DWORD count=GetModuleFileNameW(nullptr,path,ARRAYSIZE(path));
        if(!count)return false;
        if(count>=ARRAYSIZE(path)){SetLastError(ERROR_INSUFFICIENT_BUFFER);return false;}
        directory_.assign(path,count);const auto slash=directory_.find_last_of(L"\\/");
        if(slash==std::wstring::npos){SetLastError(ERROR_INVALID_NAME);return false;}
        directory_.resize(slash);return true;
    }
    void LoadBatteryCache(Device::Result& cached) {
        std::wstring path=directory_+L"\\settings-0.0.7-alpha.ini";
        const int raw=GetPrivateProfileIntW(L"Settings",L"LastRawPercent",-1,path.c_str());
        wchar_t text[64]{};GetPrivateProfileStringW(L"Settings",L"LastReadingUtc",L"0",text,ARRAYSIZE(text),path.c_str());
        ULARGE_INTEGER ticks{};ticks.QuadPart=_wcstoui64(text,nullptr,10);
        FILETIME time{ticks.LowPart,ticks.HighPart};SYSTEMTIME utc{},local{};
        if(raw>=0&&raw<=100&&ticks.QuadPart&&FileTimeToSystemTime(&time,&utc)&&
           SystemTimeToTzSpecificLocalTime(nullptr,&utc,&local)){
            cached.rawPercent=raw;cached.lastRead=local;cached.sampledAtUtc=ticks.QuadPart;
        }
    }
    void SaveBatteryCache(const Device::Result& status) {
        if(status.rawPercent<0||status.rawPercent>100||!status.sampledAtUtc||status.sampledAtUtc==lastSavedSample_)return;
        const std::wstring path=directory_+L"\\settings-0.0.7-alpha.ini";
        if(WritePrivateProfileStringW(L"Settings",L"LastRawPercent",std::to_wstring(status.rawPercent).c_str(),path.c_str())&&
           WritePrivateProfileStringW(L"Settings",L"LastReadingUtc",std::to_wstring(status.sampledAtUtc).c_str(),path.c_str()))
            lastSavedSample_=status.sampledAtUtc;
    }
    void Deliver(const Snapshot& snapshot) {
        // Copy the callable so a close initiated by the UI callback can safely clear this object's callback.
        Callback callback=callback_;if(callback)callback(snapshot);
    }
    static LRESULT CALLBACK WindowProcedure(HWND window,UINT message,WPARAM w,LPARAM l) {
        auto self=reinterpret_cast<DeviceController*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){
            self=static_cast<DeviceController*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(message==ResultMessage){
            std::unique_ptr<Snapshot> update(reinterpret_cast<Snapshot*>(l));
            if(self&&self->started_&&update){self->SaveBatteryCache(update->status);self->Deliver(*update);}
            return 0;
        }
        if(self){
            if(message==WM_DEVICECHANGE&&Device::IsTargetDeviceChange(w,l)){SetTimer(window,RefreshTimer,600,nullptr);return 0;}
            if(message==WM_TIMER&&w==RefreshTimer){KillTimer(window,RefreshTimer);self->Refresh(true);return 0;}
            if(message==WM_POWERBROADCAST&&(w==PBT_APMRESUMEAUTOMATIC||w==PBT_APMRESUMESUSPEND)){self->Refresh(true);return TRUE;}
            if(message==WM_CLOSE){if(IsWindow(self->displayWindow_))PostMessageW(self->displayWindow_,WM_CLOSE,0,0);return 0;}
        }
        if(message==WM_NCDESTROY)SetWindowLongPtrW(window,GWLP_USERDATA,0);
        return DefWindowProcW(window,message,w,l);
    }
};
