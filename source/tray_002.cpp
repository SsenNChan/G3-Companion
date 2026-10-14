#include "localization.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <dbt.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <string>
#include <memory>
#include <vector>
#include <cmath>
#include "format.h"
#include "history.h"
#include "startup.h"
#include "alpha_device.h"
#include "manager_handoff.h"
#include "monitor_settings.h"
#include "program_mode.h"
#include <windowsx.h>

using Microsoft::WRL::ComPtr;
constexpr wchar_t ClassName[]=L"PeripheralMonitor.Window";
constexpr wchar_t AppId[]=L"PeripheralMonitor.Desktop";
constexpr UINT TrayMessage=WM_APP+1, ResultMessage=WM_APP+2, ShowMessage=WM_APP+3, ManagerExitMessage=WM_APP+4;
constexpr UINT RefreshTimer=1, DeviceTimer=2;
constexpr int RefreshId=101, ExitId=102, ManagerId=106;
constexpr int ContentInset=20, PanelWidth=266, PanelHeight=166;
constexpr float RingX=205.0f, RingY=61.0f, RingRadius=37.0f, RingWidth=8.0f;
constexpr DWORD Period=600000;
constexpr wchar_t SettingsName[]=L"settings-0.0.7-alpha.ini";
constexpr wchar_t StartupName[]=L"G3 Companion 0.0.7-alpha.lnk";
constexpr wchar_t DisplayName[]=L"G3 Companion";
HWND window; HANDLE mutexHandle, stopEvent, worker;
HANDLE deviceOwnerMutex=nullptr,managerProcess=nullptr,managerWait=nullptr;
DWORD managerPid=0;
bool deviceOwned=false,managerPaused=false;
HDEVNOTIFY deviceNotice; HICON trayIcon; UINT taskbarCreated, dpi=96;
std::wstring executable, directory, settingsPath, startupPath;
bool closing=false, dark=false, trayAdded=false, settingsOk=true, notificationsOk=true, systemBackdrop=false;
bool lowBatteryNotifications=true;
constexpr int threshold=20;
bool managerHover=false,managerFocus=false;
using Availability=MouseState::Mode;
using Result=Device::Result;
enum class Level { Unknown, Critical, Low, Normal };
Result status; SYSTEMTIME lastRead{}; bool hasRead=false;
int DisplayPercent(){return MouseState::RelativePercent(status.rawPercent);}
bool HasVisiblePercent(){return MouseState::ShowPercent(status.state)&&DisplayPercent()>=0;}
struct Rule { ULONGLONG start=0; bool sent=false; } firstRule, secondRule;
ULONGLONG lastSuccess=0;
ComPtr<ID2D1Factory> graphicsFactory;
ComPtr<ID2D1HwndRenderTarget> renderTarget;
ComPtr<ID2D1SolidColorBrush> colorBrush;
ComPtr<ID2D1StrokeStyle> ringStyle;
ComPtr<IDWriteFactory> textFactory;
ComPtr<IDWriteTextFormat> titleFormat, numberFormat, labelFormat;

int Scale(int value) { return MulDiv(value,dpi,96); }
Level CurrentLevel(){return managerPaused||status.state!=Availability::Wireless?Level::Unknown:DisplayPercent()<=10?Level::Critical:DisplayPercent()<=threshold?Level::Low:Level::Normal;}
LONGLONG EpochSeconds() { FILETIME f; GetSystemTimeAsFileTime(&f); ULARGE_INTEGER n; n.LowPart=f.dwLowDateTime;n.HighPart=f.dwHighDateTime;return n.QuadPart/10000000; }
bool Put(const wchar_t* key, const std::wstring& value) { return WritePrivateProfileStringW(L"Settings",key,value.c_str(),settingsPath.c_str())!=FALSE; }
std::wstring Get(const wchar_t* key) { wchar_t b[512]{};GetPrivateProfileStringW(L"Settings",key,L"",b,512,settingsPath.c_str());return b; }
void Save() {
    bool ok=true;
    ok=Put(L"FirstSent",firstRule.sent?L"1":L"0")&&ok;
    ok=Put(L"SecondSent",secondRule.sent?L"1":L"0")&&ok;
    settingsOk=ok;
}
void Load() {
    lowBatteryNotifications=GetPrivateProfileIntW(L"Monitoring",L"LowBatteryNotificationsEnabled",1,settingsPath.c_str())==1;
    int cached=GetPrivateProfileIntW(L"Settings",L"LastRawPercent",-1,settingsPath.c_str());
    ULARGE_INTEGER ticks{};ticks.QuadPart=_wcstoui64(Get(L"LastReadingUtc").c_str(),nullptr,10);
    FILETIME time{ticks.LowPart,ticks.HighPart};SYSTEMTIME utc{},local{};
    if(cached>=0&&cached<=100&&ticks.QuadPart&&FileTimeToSystemTime(&time,&utc)&&SystemTimeToTzSpecificLocalTime(nullptr,&utc,&local)){
        status.rawPercent=cached;status.lastRead=local;status.sampledAtUtc=ticks.QuadPart;lastRead=local;hasRead=true;
    }
    wchar_t name[MAX_COMPUTERNAME_LENGTH+1]{};DWORD size=ARRAYSIZE(name);GetComputerNameW(name,&size);
    TOKEN_STATISTICS stats{};DWORD length=0;HANDLE token=nullptr;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){GetTokenInformation(token,TokenStatistics,&stats,sizeof(stats),&length);CloseHandle(token);}
    std::wstring scope=std::wstring(name)+L":"+std::to_wstring(stats.AuthenticationId.HighPart)+L":"+std::to_wstring(stats.AuthenticationId.LowPart);
    LONGLONG boot=EpochSeconds()-GetTickCount64()/1000, previous=_wtoi64(Get(L"BootTime").c_str());
    bool same=Get(L"Scope")==scope&&std::abs(boot-previous)<120;
    firstRule.sent=same&&Get(L"FirstSent")==L"1";secondRule.sent=same&&Get(L"SecondSent")==L"1";
    settingsOk=Put(L"Scope",scope)&&Put(L"BootTime",std::to_wstring(boot));Save();
}

bool Notify(bool urgent,int percent,bool test=false) {
    NOTIFYICONDATAW data{sizeof(data)};data.hWnd=window;data.uID=1;data.uFlags=NIF_INFO;
    data.dwInfoFlags=NIIF_WARNING|NIIF_RESPECT_QUIET_TIME;
    std::wstring title=test?Localization::Get(Localization::T261):urgent?Localization::Get(Localization::T262):Localization::Get(Localization::T263);
    std::wstring text=test?Localization::Get(Localization::T264):Localization::Format(Localization::T265,{{L"percent",std::to_wstring(percent)}});
    wcsncpy_s(data.szInfoTitle,title.c_str(),_TRUNCATE);wcsncpy_s(data.szInfo,text.c_str(),_TRUNCATE);
    notificationsOk=Shell_NotifyIconW(NIM_MODIFY,&data)!=FALSE;
    return notificationsOk;
}

void Refresh(){if(!closing&&!managerPaused&&worker)SetEvent(Device::refresh);}
void InvalidateConnection(){Device::generation.fetch_add(1);status.state=Availability::Unknown;InvalidateRect(window,nullptr,FALSE);}
void ResetPending(){firstRule.start=secondRule.start=0;lastSuccess=0;}
void Evaluate(int percent) {
    if(!lowBatteryNotifications){ResetPending();return;}
    ULONGLONG now=GetTickCount64();
    if(lastSuccess&&now-lastSuccess>Period+60000)firstRule.start=secondRule.start=0;
    lastSuccess=now;
    if(percent>=threshold+10){bool changed=firstRule.sent||secondRule.sent;firstRule={};secondRule={};if(changed)Save();}
    auto advance=[&](Rule& rule,int limit){if(percent>limit)rule.start=0;else if(!rule.sent&&!rule.start)rule.start=now;};
    advance(firstRule,threshold);advance(secondRule,10);
    bool urgent=!secondRule.sent&&secondRule.start&&now-secondRule.start>=600000;
    bool initial=!firstRule.sent&&firstRule.start&&now-firstRule.start>=3600000;
    if(urgent){if(Notify(true,percent)){secondRule.sent=true;firstRule.sent=true;Save();}}
    else if(initial&&percent>10){if(Notify(false,percent)){firstRule.sent=true;Save();}}
}

bool IsDark(){DWORD value=1,size=sizeof(value);RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&value,&size);return value==0;}
HRESULT InitializeGraphics(){
    HRESULT hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,graphicsFactory.GetAddressOf());if(FAILED(hr))return hr;
    D2D1_STROKE_STYLE_PROPERTIES stroke=D2D1::StrokeStyleProperties();
    stroke.startCap=stroke.endCap=D2D1_CAP_STYLE_ROUND;
    return graphicsFactory->CreateStrokeStyle(stroke,nullptr,0,ringStyle.GetAddressOf());
}
HRESULT InitializeTextResources(){
    if(textFactory)return S_OK;
    // Tray icons only need Direct2D; create font resources when the flyout is opened.
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteTextFormat> title,number,label;
    HRESULT hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf()));if(FAILED(hr))return hr;
    auto format=[&](float size,DWRITE_FONT_WEIGHT weight,ComPtr<IDWriteTextFormat>& target){
        HRESULT result=factory->CreateTextFormat(Localization::IsEnglish()?L"Segoe UI":L"Microsoft YaHei",nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,Localization::LanguageTag(),target.GetAddressOf());
        if(SUCCEEDED(result)){target->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);target->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);}return result;
    };
    if(FAILED(hr=format(14,DWRITE_FONT_WEIGHT_SEMI_BOLD,title)))return hr;
    if(FAILED(hr=format(44,DWRITE_FONT_WEIGHT_SEMI_BOLD,number)))return hr;
    if(FAILED(hr=format(12,DWRITE_FONT_WEIGHT_NORMAL,label)))return hr;
    textFactory=std::move(factory);titleFormat=std::move(title);numberFormat=std::move(number);labelFormat=std::move(label);
    return S_OK;
}
HRESULT CreateBatteryGeometry(int percent,ComPtr<ID2D1PathGeometry>& geometry){
    if(!graphicsFactory)return E_UNEXPECTED;
    HRESULT result=graphicsFactory->CreatePathGeometry(geometry.GetAddressOf());
    if(FAILED(result))return result;
    ComPtr<ID2D1GeometrySink> sink;
    result=geometry->Open(sink.GetAddressOf());if(FAILED(result))return result;
    sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
    // Fluent Battery regular (20px): outline and an inset capacity fill, separated by a transparent gap.
    // https://github.com/microsoft/fluentui-system-icons/tree/main/assets/Battery%200
    sink->BeginFigure(D2D1::Point2F(16,5),D2D1_FIGURE_BEGIN_FILLED);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(17.6569f,5),D2D1::Point2F(19,6.34315f),D2D1::Point2F(19,8)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(19.5523f,8),D2D1::Point2F(20,8.44772f),D2D1::Point2F(20,9)));
    sink->AddLine(D2D1::Point2F(20,11));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(20,11.5523f),D2D1::Point2F(19.5523f,12),D2D1::Point2F(19,12)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(19,13.6569f),D2D1::Point2F(17.6569f,15),D2D1::Point2F(16,15)));
    sink->AddLine(D2D1::Point2F(3,15));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(1.34315f,15),D2D1::Point2F(0,13.6569f),D2D1::Point2F(0,12)));
    sink->AddLine(D2D1::Point2F(0,8));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(0,6.34315f),D2D1::Point2F(1.34315f,5),D2D1::Point2F(3,5)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    auto rounded=[&](float left,float top,float right,float bottom,float radius){
        float corner=radius*0.55228475f;
        sink->BeginFigure(D2D1::Point2F(left+radius,top),D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(right-radius,top));
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(right-radius+corner,top),D2D1::Point2F(right,top+radius-corner),D2D1::Point2F(right,top+radius)));
        sink->AddLine(D2D1::Point2F(right,bottom-radius));
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(right,bottom-radius+corner),D2D1::Point2F(right-radius+corner,bottom),D2D1::Point2F(right-radius,bottom)));
        sink->AddLine(D2D1::Point2F(left+radius,bottom));
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(left+radius-corner,bottom),D2D1::Point2F(left,bottom-radius+corner),D2D1::Point2F(left,bottom-radius)));
        sink->AddLine(D2D1::Point2F(left,top+radius));
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(left,top+radius-corner),D2D1::Point2F(left+radius-corner,top),D2D1::Point2F(left+radius,top)));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    };
    rounded(1,6,18,14,2);
    float right=2+15*std::clamp(percent,0,100)/100.0f;
    if(right>2)rounded(2,7,right,13,std::min(1.0f,(right-2)/2));
    return sink->Close();
}
HICON MakeIcon(int percent,Level level,int n){
    bool valid=level!=Level::Unknown;
    ComPtr<ID2D1PathGeometry> geometry;
    if(FAILED(CreateBatteryGeometry(valid?percent:0,geometry)))return nullptr;
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=n;bi.bmiHeader.biHeight=-n;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    DWORD* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(nullptr,&bi,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);if(!bitmap)return nullptr;
    HDC dc=CreateCompatibleDC(nullptr);if(!dc){DeleteObject(bitmap);return nullptr;}
    HGDIOBJ previous=SelectObject(dc,bitmap);
    if(!previous || previous==HGDI_ERROR){DeleteDC(dc);DeleteObject(bitmap);return nullptr;}
    ZeroMemory(pixels,static_cast<size_t>(n)*n*sizeof(DWORD));
    HRESULT result=[&]()->HRESULT{
        ComPtr<ID2D1DCRenderTarget> target;
        auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        HRESULT hr=graphicsFactory->CreateDCRenderTarget(&properties,target.GetAddressOf());if(FAILED(hr))return hr;
        RECT bounds{0,0,n,n};hr=target->BindDC(dc,&bounds);if(FAILED(hr))return hr;
        DWORD color=!valid?0x999999:level==Level::Critical?0xe45c5c:level==Level::Low?0xd89524:dark?0xeeeeee:0x303030;
        ComPtr<ID2D1SolidColorBrush> brush;
        hr=target->CreateSolidColorBrush(D2D1::ColorF(color),brush.GetAddressOf());if(FAILED(hr))return hr;
        target->BeginDraw();target->Clear(D2D1::ColorF(0,0,0,0));
        target->SetTransform(D2D1::Matrix3x2F::Scale(n/20.0f,n/20.0f));
        target->FillGeometry(geometry.Get(),brush.Get());
        if(!valid)target->DrawLine(D2D1::Point2F(3,12.5f),D2D1::Point2F(16,7.5f),brush.Get(),1.4f,ringStyle.Get());
        return target->EndDraw();
    }();
    SelectObject(dc,previous);DeleteDC(dc);
    HICON icon=nullptr;
    if(SUCCEEDED(result)){
        std::vector<BYTE> maskPixels(static_cast<size_t>((n+15)/16)*2*n,0);
        HBITMAP mask=CreateBitmap(n,n,1,1,maskPixels.data());
        if(mask){ICONINFO info{TRUE,0,0,mask,bitmap};icon=CreateIconIndirect(&info);DeleteObject(mask);}
    }
    DeleteObject(bitmap);return icon;
}
std::wstring ReadTime(bool compact=false){wchar_t b[32];wchar_t* end=b;SYSTEMTIME today;GetLocalTime(&today);
    if(today.wYear==lastRead.wYear&&today.wMonth==lastRead.wMonth&&today.wDay==lastRead.wDay)end=Format::Time(end,lastRead);
    else if(compact){end=Format::Number(end,lastRead.wMonth,2);*end++=L'/';end=Format::Number(end,lastRead.wDay,2);}
    else{end=Format::Date(end,lastRead);*end++=L' ';end=Format::Time(end,lastRead);}
    return std::wstring(b,end);
}
std::wstring StatusLabel(){
    if(managerPaused)return Localization::Get(Localization::T266);
    switch(status.state){
    case Availability::Wireless:return Localization::Get(Localization::T008);
    case Availability::Wired:return Localization::Get(Localization::T005);
    case Availability::Missing:return Localization::Get(Localization::T006);
    case Availability::Charging:return Localization::Get(Localization::T250);
    case Availability::AwaitingBattery:return Localization::Get(Localization::T009);
    case Availability::Offline:return hasRead?Localization::Format(Localization::T248,{{L"time",ReadTime(true)}}):Localization::Get(Localization::T249);
    case Availability::Error:return Localization::Get(Localization::T046);
    default:return Localization::Get(Localization::T252);
    }
}
void UpdateTray();
void ClearManagerWatch(){
    if(managerWait){UnregisterWaitEx(managerWait,INVALID_HANDLE_VALUE);managerWait=nullptr;}
    if(managerProcess){CloseHandle(managerProcess);managerProcess=nullptr;}managerPid=0;
}
VOID CALLBACK ManagerExited(void* context,BOOLEAN){
    PostMessageW(window,ManagerExitMessage,static_cast<WPARAM>(reinterpret_cast<ULONG_PTR>(context)),0);
}
bool WatchManager(HANDLE process,DWORD pid){
    if(managerProcess&&managerPid==pid&&WaitForSingleObject(managerProcess,0)==WAIT_TIMEOUT)return true;
    if(managerProcess&&WaitForSingleObject(managerProcess,0)==WAIT_TIMEOUT){SetLastError(ERROR_BUSY);return false;}
    ClearManagerWatch();
    if(!DuplicateHandle(GetCurrentProcess(),process,GetCurrentProcess(),&managerProcess,SYNCHRONIZE,FALSE,0))return false;
    managerPid=pid;
    if(!RegisterWaitForSingleObject(&managerWait,managerProcess,ManagerExited,reinterpret_cast<void*>(static_cast<ULONG_PTR>(pid)),INFINITE,WT_EXECUTEONLYONCE)){
        const DWORD error=GetLastError();managerWait=nullptr;ClearManagerWatch();SetLastError(error);return false;
    }
    return true;
}
void PauseDevice(){
    ProgramLog::Write("device_monitoring_paused","reason=manager");
    managerPaused=true;Device::generation.fetch_add(1);ResetPending();KillTimer(window,DeviceTimer);
    SetEvent(stopEvent);
    if(worker){CancelSynchronousIo(worker);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);worker=nullptr;}
    if(deviceOwned){ReleaseMutex(deviceOwnerMutex);deviceOwned=false;}
    UpdateTray();InvalidateRect(window,nullptr,FALSE);
}
bool StartDevice(){
    if(closing)return false;
    if(worker)return true;
    if(HWND active=Handoff::FindManager(directory)){
        DWORD pid=0;HANDLE process=Handoff::OpenManagerProcess(active,directory,pid);
        bool tracked=process&&WatchManager(process,pid);if(process)CloseHandle(process);
        if(!tracked)return false;
        managerPaused=true;UpdateTray();InvalidateRect(window,nullptr,FALSE);return true;
    }
    DWORD acquired=WaitForSingleObject(deviceOwnerMutex,0);
    if(acquired!=WAIT_OBJECT_0&&acquired!=WAIT_ABANDONED){SetLastError(ERROR_BUSY);return false;}
    deviceOwned=true;managerPaused=false;ResetEvent(stopEvent);ResetEvent(Device::refresh);
    status.state=Availability::Unknown;Device::initial=status;
    worker=CreateThread(nullptr,0,Device::Run,nullptr,0,nullptr);
    if(!worker){DWORD error=GetLastError();ReleaseMutex(deviceOwnerMutex);deviceOwned=false;SetLastError(error);return false;}
    ProgramLog::Write("device_monitoring_started");
    Refresh();UpdateTray();InvalidateRect(window,nullptr,FALSE);return true;
}
bool AcceptManager(HWND managerWindow,DWORD claimedPid){
    if(closing)return false;
    DWORD pid=0;HANDLE process=Handoff::OpenManagerProcess(managerWindow,directory,pid);
    if(!process)return false;
    bool accepted=pid==claimedPid&&WatchManager(process,pid);CloseHandle(process);
    if(!accepted)return false;
    PauseDevice();return true;
}
void ResumeDevice(){
    ClearManagerWatch();managerPaused=false;
    if(!StartDevice()){
        status.state=Availability::Error;status.error=GetLastError();UpdateTray();InvalidateRect(window,nullptr,FALSE);
        MessageBoxW(window,Localization::Get(Localization::T267),DisplayName,MB_OK|MB_ICONERROR);
    }
}
void OpenManager(){
    if(HWND active=Handoff::FindManager(directory)){
        DWORD pid=0;GetWindowThreadProcessId(active,&pid);
        if(!AcceptManager(active,pid)){MessageBoxW(window,Localization::Get(Localization::T268),DisplayName,MB_OK|MB_ICONERROR);return;}
        Handoff::ActivateManager(active);ShowWindow(window,SW_HIDE);return;
    }
    if(managerProcess&&WaitForSingleObject(managerProcess,0)==WAIT_TIMEOUT)return;
    ClearManagerWatch();PauseDevice();
    std::wstring path=directory+L"\\"+Handoff::ManagerFilename;
    std::wstring command=L"\""+path+L"\""+Localization::LaunchArgument();
    STARTUPINFOW start{sizeof(start)};PROCESS_INFORMATION process{};
    if(!CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,directory.c_str(),&start,&process)){
        DWORD error=GetLastError();ResumeDevice();
        MessageBoxW(window,(Localization::Format(Localization::T269,{{L"errorCode",std::to_wstring(error)}})).c_str(),DisplayName,MB_OK|MB_ICONERROR);return;
    }
    CloseHandle(process.hThread);
    bool tracked=WatchManager(process.hProcess,process.dwProcessId);CloseHandle(process.hProcess);
    if(!tracked){
        MessageBoxW(window,Localization::Get(Localization::T270),DisplayName,MB_OK|MB_ICONERROR);
        DestroyWindow(window);return;
    }
    ShowWindow(window,SW_HIDE);
}
void UpdateTray(){
    static int iconPercent=-2,iconSize=0;static Level iconLevel=Level::Unknown;static bool iconDark=false,iconPending=true;
    Level level=CurrentLevel();int n=std::max(GetSystemMetricsForDpi(SM_CXSMICON,dpi),16);
    bool changed=!trayIcon||iconPercent!=DisplayPercent()||iconLevel!=level||iconDark!=dark||iconSize!=n;
    HICON icon=changed?MakeIcon(DisplayPercent(),level,n):trayIcon;
    if(changed)iconPending=true;
    NOTIFYICONDATAW data{sizeof(data)};data.hWnd=window;data.uID=1;data.uFlags=NIF_MESSAGE|NIF_TIP|NIF_SHOWTIP;data.uCallbackMessage=TrayMessage;
    if(!trayAdded||iconPending)data.uFlags|=NIF_ICON;data.hIcon=icon;
    std::wstring tip;
    if(managerPaused)tip=Localization::Get(Localization::T271);
    else switch(status.state){
    case Availability::Offline:tip=Localization::Get(Localization::T272);break;
    case Availability::Missing:tip=Localization::Get(Localization::T273);break;
    case Availability::Wired:tip=Localization::Get(Localization::T005);break;
    case Availability::Error:tip=Localization::Get(Localization::T274);break;
    case Availability::AwaitingBattery:tip=Localization::Get(Localization::T251);break;
    default:tip=Localization::Format(Localization::T275,{{L"percent",DisplayPercent()>=0?std::to_wstring(DisplayPercent()):std::wstring(L"--")}});break;
    }
    wcsncpy_s(data.szTip,tip.c_str(),_TRUNCATE);
    bool updated=false;
    if(!trayAdded){trayAdded=Shell_NotifyIconW(NIM_ADD,&data)!=FALSE;updated=trayAdded;if(trayAdded){data.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&data);}}
    else updated=Shell_NotifyIconW(NIM_MODIFY,&data)!=FALSE;
    if(updated)iconPending=false;
    if(changed){if(trayIcon)DestroyIcon(trayIcon);trayIcon=icon;iconPercent=DisplayPercent();iconLevel=level;iconDark=dark;iconSize=n;}
}
struct Palette { D2D1_COLOR_F background,foreground,secondary,track; };
D2D1_COLOR_F Color(unsigned value){return D2D1::ColorF(((value>>16)&255)/255.0f,((value>>8)&255)/255.0f,(value&255)/255.0f);}
Palette Colors(){return dark?Palette{Color(0x0b0b0d),Color(0xf5f5f7),Color(0xa6a6ac),Color(0x48484d)}
    :Palette{Color(0xf3f4f6),Color(0x202124),Color(0x686a70),Color(0xe4e6e9)};}
void Theme(){
    dark=IsDark();BOOL use=dark;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&use,sizeof(use));
    DWM_WINDOW_CORNER_PREFERENCE corner=DWMWCP_ROUND;DwmSetWindowAttribute(window,DWMWA_WINDOW_CORNER_PREFERENCE,&corner,sizeof(corner));
    COLORREF border=dark?RGB(53,53,58):RGB(226,228,232);DwmSetWindowAttribute(window,DWMWA_BORDER_COLOR,&border,sizeof(border));
    DWM_SYSTEMBACKDROP_TYPE type=DWMSBT_TRANSIENTWINDOW;
    bool enabled=SUCCEEDED(DwmSetWindowAttribute(window,DWMWA_SYSTEMBACKDROP_TYPE,&type,sizeof(type)));
    if(enabled){MARGINS margins{-1,-1,-1,-1};enabled=SUCCEEDED(DwmExtendFrameIntoClientArea(window,&margins));}
    if(!enabled){type=DWMSBT_NONE;DwmSetWindowAttribute(window,DWMWA_SYSTEMBACKDROP_TYPE,&type,sizeof(type));}
    if(enabled!=systemBackdrop){systemBackdrop=enabled;colorBrush.Reset();renderTarget.Reset();}
    UpdateTray();InvalidateRect(window,nullptr,FALSE);
}
HRESULT Resources(){
    HRESULT hr=InitializeTextResources();if(FAILED(hr))return hr;
    if(renderTarget)return S_OK;
    RECT area{};GetClientRect(window,&area);
    auto format=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,systemBackdrop?D2D1_ALPHA_MODE_PREMULTIPLIED:D2D1_ALPHA_MODE_IGNORE);
    auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,format);
    hr=graphicsFactory->CreateHwndRenderTarget(properties,D2D1::HwndRenderTargetProperties(window,D2D1::SizeU(area.right,area.bottom)),renderTarget.GetAddressOf());
    if(FAILED(hr))return hr;
    renderTarget->SetDpi(static_cast<float>(dpi),static_cast<float>(dpi));
    hr=renderTarget->CreateSolidColorBrush(Color(0xffffff),colorBrush.GetAddressOf());
    if(FAILED(hr))renderTarget.Reset();return hr;
}
void DrawLabel(const std::wstring& value,IDWriteTextFormat* format,float left,float top,float right,float bottom,D2D1_COLOR_F color){
    colorBrush->SetColor(color);renderTarget->DrawText(value.c_str(),static_cast<UINT32>(value.size()),format,D2D1::RectF(left,top,right,bottom),colorBrush.Get());
}
void Paint(){
    PAINTSTRUCT ps;BeginPaint(window,&ps);
    if(!IsWindowVisible(window)){EndPaint(window,&ps);return;}
    if(SUCCEEDED(Resources())){
        Palette palette=Colors();renderTarget->BeginDraw();renderTarget->Clear(systemBackdrop?D2D1::ColorF(0,0,0,0):palette.background);
        if(systemBackdrop){colorBrush->SetColor(dark?D2D1::ColorF(0.035f,0.035f,0.045f,0.58f):D2D1::ColorF(0.97f,0.97f,0.98f,0.72f));renderTarget->FillRectangle(D2D1::RectF(0,0,PanelWidth,PanelHeight),colorBrush.Get());}
        DrawLabel(Localization::Get(Localization::T040),titleFormat.Get(),ContentInset,14,RingX-RingRadius-ContentInset,38,palette.foreground);
        std::wstring value=HasVisiblePercent()?std::to_wstring(DisplayPercent())+L"%":L"--%";
        DrawLabel(value,numberFormat.Get(),ContentInset,34,RingX-RingRadius-ContentInset,87,palette.foreground);
        std::wstring label=StatusLabel();
        Level current=CurrentLevel();
        D2D1_COLOR_F level=current==Level::Unknown?palette.secondary:current==Level::Critical?Color(dark?0xff686e:0xd1434c):current==Level::Low?Color(dark?0xf4b54b:0xb7791e):Color(dark?0x42d47c:0x168f57);
        D2D1_COLOR_F stateColor=status.state==Availability::Missing||status.state==Availability::Error?Color(dark?0xff686e:0xd1434c):palette.secondary;
        if(status.state==Availability::Charging)stateColor=Color(dark?0x42d47c:0x168f57);
        DrawLabel(label,labelFormat.Get(),ContentInset,86,RingX-RingRadius-ContentInset,122,stateColor);
        auto ring=D2D1::Ellipse(D2D1::Point2F(RingX,RingY),RingRadius,RingRadius);
        colorBrush->SetColor(palette.track);renderTarget->DrawEllipse(ring,colorBrush.Get(),RingWidth);
        if(HasVisiblePercent()){
            colorBrush->SetColor(level);
            if(DisplayPercent()==100)renderTarget->DrawEllipse(ring,colorBrush.Get(),RingWidth);
            else if(DisplayPercent()==0)renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(RingX,RingY-RingRadius),RingWidth/2,RingWidth/2),colorBrush.Get());
            else if(DisplayPercent()>0){
                ComPtr<ID2D1PathGeometry> path;ComPtr<ID2D1GeometrySink> sink;
                if(SUCCEEDED(graphicsFactory->CreatePathGeometry(path.GetAddressOf()))&&SUCCEEDED(path->Open(sink.GetAddressOf()))){
                    float angle=static_cast<float>(DisplayPercent())*6.2831853f/100.0f;
                    sink->BeginFigure(D2D1::Point2F(RingX,RingY-RingRadius),D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(RingX+RingRadius*std::sin(angle),RingY-RingRadius*std::cos(angle)),D2D1::SizeF(RingRadius,RingRadius),0,D2D1_SWEEP_DIRECTION_CLOCKWISE,DisplayPercent()>50?D2D1_ARC_SIZE_LARGE:D2D1_ARC_SIZE_SMALL));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    if(SUCCEEDED(sink->Close()))renderTarget->DrawGeometry(path.Get(),colorBrush.Get(),RingWidth,ringStyle.Get());
                }
            }
        }
        if(status.state==Availability::Charging){
            ComPtr<ID2D1PathGeometry> path;ComPtr<ID2D1GeometrySink> sink;
            if(SUCCEEDED(graphicsFactory->CreatePathGeometry(path.GetAddressOf()))&&SUCCEEDED(path->Open(sink.GetAddressOf()))){
                sink->BeginFigure(D2D1::Point2F(RingX+4,RingY-19),D2D1_FIGURE_BEGIN_FILLED);
                const D2D1_POINT_2F points[]={{RingX-12,RingY+3},{RingX-2,RingY+3},{RingX-5,RingY+19},{RingX+12,RingY-4},{RingX+2,RingY-4}};
                sink->AddLines(points,ARRAYSIZE(points));sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if(SUCCEEDED(sink->Close())){colorBrush->SetColor(Color(dark?0x42d47c:0x168f57));renderTarget->FillGeometry(path.Get(),colorBrush.Get());}
            }
        }
        colorBrush->SetColor(dark?D2D1::ColorF(1,1,1,0.20f):D2D1::ColorF(0,0,0,0.18f));renderTarget->DrawLine(D2D1::Point2F(ContentInset,122),D2D1::Point2F(PanelWidth-ContentInset,122),colorBrush.Get(),1);
        if(managerHover||managerFocus){colorBrush->SetColor(dark?D2D1::ColorF(1,1,1,0.10f):D2D1::ColorF(0,0,0,0.06f));renderTarget->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(8,127,PanelWidth-8,159),5,5),colorBrush.Get());}
        DrawLabel(Localization::Get(Localization::T276),labelFormat.Get(),ContentInset,128,220,158,palette.foreground);
        DrawLabel(L">",labelFormat.Get(),239,128,250,158,palette.secondary);
        HRESULT hr=renderTarget->EndDraw();if(hr==D2DERR_RECREATE_TARGET){colorBrush.Reset();renderTarget.Reset();InvalidateRect(window,nullptr,FALSE);}
    }
    EndPaint(window,&ps);
}
void ShowFlyout(bool toggle){if(toggle&&IsWindowVisible(window)){ShowWindow(window,SW_HIDE);return;}
    if(FAILED(InitializeTextResources())){MessageBoxW(window,Localization::Get(Localization::T277),Localization::Get(Localization::T278),MB_OK|MB_ICONERROR);DestroyWindow(window);return;}
    NOTIFYICONIDENTIFIER id{sizeof(id)};id.hWnd=window;id.uID=1;RECT anchor{};POINT cursor;GetCursorPos(&cursor);
    if(FAILED(Shell_NotifyIconGetRect(&id,&anchor)))anchor={cursor.x,cursor.y,cursor.x+1,cursor.y+1};
    HMONITOR monitor=MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(monitor,&mi);
    SetWindowPos(window,nullptr,anchor.left,anchor.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);dpi=GetDpiForWindow(window);if(renderTarget)renderTarget->SetDpi(static_cast<float>(dpi),static_cast<float>(dpi));
    int width=Scale(PanelWidth),height=Scale(PanelHeight),gap=Scale(10);
    int x=anchor.right-width,y=anchor.top-height-gap;if(y<mi.rcWork.top)y=anchor.bottom+gap;
    x=std::clamp<LONG>(x,mi.rcWork.left,std::max(mi.rcWork.left,mi.rcWork.right-width));y=std::clamp<LONG>(y,mi.rcWork.top,std::max(mi.rcWork.top,mi.rcWork.bottom-height));
    SetWindowPos(window,HWND_TOPMOST,x,y,width,height,SWP_SHOWWINDOW);
    if(systemBackdrop){MARGINS margins{-1,-1,-1,-1};DwmExtendFrameIntoClientArea(window,&margins);}
    SetForegroundWindow(window);SetFocus(window);Refresh();
}
Startup::State InspectStartup(){return Startup::Inspect(startupPath,executable,directory,L"--tray");}
void RepairStartup(){
    if(InspectStartup()==Startup::State::RepairNeeded&&FAILED(Startup::Repair(startupPath,executable,directory,L"--tray")))
        MessageBoxW(window,Localization::Get(Localization::T279),DisplayName,MB_OK|MB_ICONERROR);
}
HMENU CreateTrayMenu(){
    HMENU menu=CreatePopupMenu();
    if(!menu)return nullptr;
    MENUINFO info{sizeof(info)};info.fMask=MIM_STYLE;info.dwStyle=MNS_NOCHECK;
    SetMenuInfo(menu,&info);
    AppendMenuW(menu,MF_STRING|(managerPaused?MF_GRAYED:0),RefreshId,Localization::Get(Localization::T280));
    AppendMenuW(menu,MF_STRING,ExitId,Localization::Get(Localization::T281));
    return menu;
}
void Menu(){
    HMENU menu=CreateTrayMenu();if(!menu)return;
    POINT p;GetCursorPos(&p);SetForegroundWindow(window);int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);
    DestroyMenu(menu);PostMessageW(window,WM_NULL,0,0);if(id)SendMessageW(window,WM_COMMAND,id,0);
}
LRESULT CALLBACK Procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l){
    if(message==taskbarCreated&&taskbarCreated){trayAdded=false;UpdateTray();return 0;}
    if(message==Handoff::RequestMessage())return AcceptManager(reinterpret_cast<HWND>(w),static_cast<DWORD>(l))?1:0;
    if(message==Handoff::StopTrayMessage()){PostMessageW(hwnd,WM_COMMAND,ExitId,0);return 1;}
    if(message==Handoff::TrayStateMessage())return managerPaused&&!worker&&!deviceOwned?2:worker&&deviceOwned?1:0;
    if(message==Handoff::NotificationsChangedMessage()){
        lowBatteryNotifications=GetPrivateProfileIntW(L"Monitoring",L"LowBatteryNotificationsEnabled",1,settingsPath.c_str())==1;
        ResetPending();return 1;
    }
    if(message==Handoff::TestNotificationMessage())return Notify(false,0,true)?1:0;
    switch(message){
    case WM_CREATE:return 0;
    case WM_NCCALCSIZE:if(w)return 0;break;
    case WM_PAINT:Paint();return 0;
    case WM_ERASEBKGND:return 1;
    case WM_SHOWWINDOW:
        if(!w){colorBrush.Reset();renderTarget.Reset();titleFormat.Reset();numberFormat.Reset();labelFormat.Reset();textFactory.Reset();}
        break;
    case WM_ACTIVATE:if(LOWORD(w)==WA_INACTIVE)ShowWindow(hwnd,SW_HIDE);return 0;
    case WM_CLOSE:ShowWindow(hwnd,SW_HIDE);return 0;
    case WM_KEYDOWN:
        if(w==VK_ESCAPE){ShowWindow(hwnd,SW_HIDE);return 0;}
        if(w==VK_TAB){managerFocus=!managerFocus;InvalidateRect(hwnd,nullptr,FALSE);return 0;}
        if((w==VK_RETURN||w==VK_SPACE)&&managerFocus){OpenManager();return 0;}break;
    case WM_MOUSEMOVE:{bool hover=GET_Y_LPARAM(l)>=Scale(123)&&GET_Y_LPARAM(l)<Scale(PanelHeight);
        if(hover!=managerHover){managerHover=hover;InvalidateRect(hwnd,nullptr,FALSE);}
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);return 0;}
    case WM_MOUSELEAVE:managerHover=false;InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_SETCURSOR:if(LOWORD(l)==HTCLIENT&&managerHover){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}break;
    case WM_LBUTTONUP:if(GET_Y_LPARAM(l)>=Scale(123)&&GET_Y_LPARAM(l)<Scale(PanelHeight)){OpenManager();return 0;}break;
    case WM_SETTINGCHANGE:Theme();return 0;
    case WM_DPICHANGED:{dpi=HIWORD(w);if(renderTarget)renderTarget->SetDpi(static_cast<float>(dpi),static_cast<float>(dpi));auto suggested=reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd,nullptr,suggested->left,suggested->top,Scale(PanelWidth),Scale(PanelHeight),SWP_NOZORDER|SWP_NOACTIVATE);UpdateTray();InvalidateRect(hwnd,nullptr,FALSE);return 0;}
    case WM_SIZE:if(renderTarget&&LOWORD(l)&&HIWORD(l))renderTarget->Resize(D2D1::SizeU(LOWORD(l),HIWORD(l)));return 0;
    case TrayMessage:switch(LOWORD(l)){case NIN_SELECT:case NIN_KEYSELECT:ShowFlyout(true);break;case NIN_BALLOONUSERCLICK:ShowFlyout(false);break;case WM_CONTEXTMENU:Menu();break;}return 0;
    case ShowMessage:ShowFlyout(false);return 0;
    case ManagerExitMessage:
        if(managerProcess&&managerPid==static_cast<DWORD>(w)&&WaitForSingleObject(managerProcess,0)==WAIT_OBJECT_0)ResumeDevice();return 0;
    case WM_TIMER:if(w==DeviceTimer)KillTimer(hwnd,DeviceTimer);Refresh();return 0;
    case WM_DEVICECHANGE:if(Device::IsTargetDeviceChange(w,l)){InvalidateConnection();ResetPending();SetTimer(hwnd,DeviceTimer,600,nullptr);}return 0;
    case WM_POWERBROADCAST:if(w==PBT_APMSUSPEND){InvalidateConnection();ResetPending();}else if(w==PBT_APMRESUMEAUTOMATIC||w==PBT_APMRESUMESUSPEND){InvalidateConnection();ResetPending();SetTimer(hwnd,DeviceTimer,1000,nullptr);}return TRUE;
    case ResultMessage:{std::unique_ptr<Result> result(reinterpret_cast<Result*>(l));
        if(closing||managerPaused||!result||result->generation!=Device::generation.load())return 0;
        ULONGLONG previousSample=status.sampledAtUtc;
        status=*result;hasRead=status.rawPercent>=0;lastRead=status.lastRead;
        if(status.sampledAtUtc&&status.sampledAtUtc!=previousSample){
            bool saved=Put(L"LastRawPercent",std::to_wstring(status.rawPercent));
            settingsOk=Put(L"LastReadingUtc",std::to_wstring(status.sampledAtUtc))&&saved;
        }
        if(status.state==Availability::Wireless)Evaluate(DisplayPercent());else ResetPending();
        UpdateTray();InvalidateRect(hwnd,nullptr,FALSE);return 0;}
    case WM_COMMAND:
        if(LOWORD(w)==ManagerId){OpenManager();return 0;}
        if(LOWORD(w)==ExitId){DestroyWindow(hwnd);return 0;}if(LOWORD(w)==RefreshId){Refresh();return 0;}
        break;
    case WM_QUERYENDSESSION:return TRUE;
    case WM_ENDSESSION:if(w)DestroyWindow(hwnd);return 0;
    case WM_DESTROY:closing=true;ClearManagerWatch();KillTimer(hwnd,RefreshTimer);KillTimer(hwnd,DeviceTimer);SetEvent(stopEvent);if(worker){CancelSynchronousIo(worker);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);worker=nullptr;}
        if(deviceOwned){ReleaseMutex(deviceOwnerMutex);deviceOwned=false;}
        {MSG pending;while(PeekMessageW(&pending,hwnd,ResultMessage,ResultMessage,PM_REMOVE)){delete reinterpret_cast<Result*>(pending.lParam);} }
        if(deviceNotice)UnregisterDeviceNotification(deviceNotice);{NOTIFYICONDATAW data{sizeof(data)};data.hWnd=hwnd;data.uID=1;Shell_NotifyIconW(NIM_DELETE,&data);}PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,message,w,l);
}
#ifdef MCHOSE_UNIFIED_ENTRY
int TrayProgramMain(HINSTANCE h,HINSTANCE,PWSTR,int){
#else
int WINAPI wWinMain(HINSTANCE h,HINSTANCE,PWSTR,int){
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    mutexHandle=CreateMutexW(nullptr,FALSE,L"Local\\PeripheralMonitor.Singleton");if(!mutexHandle)return 1;
    if(GetLastError()==ERROR_ALREADY_EXISTS){if(HWND previous=FindWindowW(ClassName,nullptr)){DWORD pid;GetWindowThreadProcessId(previous,&pid);AllowSetForegroundWindow(pid);PostMessageW(previous,ShowMessage,0,0);}CloseHandle(mutexHandle);return 0;}
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))){MessageBoxW(nullptr,Localization::Get(Localization::T282),Localization::Get(Localization::T278),MB_OK|MB_ICONERROR);CloseHandle(mutexHandle);return 1;}
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,ARRAYSIZE(path));executable=path;directory=executable.substr(0,executable.find_last_of(L"\\"));settingsPath=directory+L"\\"+SettingsName;
    if(GetPrivateProfileIntW(L"Monitoring",L"TrayEnabled",0,settingsPath.c_str())!=1){
        CloseHandle(mutexHandle);CoUninitialize();return 0;
    }
    PWSTR folder=nullptr;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup,0,nullptr,&folder))){startupPath=std::wstring(folder)+L"\\"+StartupName;CoTaskMemFree(folder);}
    {ProgramLog::Configure(directory,settingsPath,"tray");ProgramLog::Write("program_started");}
    SetCurrentProcessExplicitAppUserModelID(AppId);Load();
    stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!stopEvent)return 1;
    taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");WNDCLASSW wc{};wc.lpfnWndProc=Procedure;wc.hInstance=h;wc.lpszClassName=ClassName;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(h,MAKEINTRESOURCEW(1));RegisterClassW(&wc);
    window=CreateWindowExW(WS_EX_TOOLWINDOW,ClassName,DisplayName,WS_POPUP|WS_CAPTION,0,0,PanelWidth,PanelHeight,nullptr,nullptr,h,nullptr);
    if(!window){MessageBoxW(nullptr,Localization::Get(Localization::T283),Localization::Get(Localization::T278),MB_OK|MB_ICONERROR);CloseHandle(stopEvent);CloseHandle(mutexHandle);return 1;}
    dpi=GetDpiForWindow(window);if(FAILED(InitializeGraphics())){MessageBoxW(window,Localization::Get(Localization::T277),Localization::Get(Localization::T278),MB_OK|MB_ICONERROR);DestroyWindow(window);CloseHandle(stopEvent);CloseHandle(mutexHandle);CoUninitialize();return 1;}Theme();
    {
        RepairStartup();
        GUID guid;HidD_GetHidGuid(&guid);DEV_BROADCAST_DEVICEINTERFACE_W filter{};filter.dbcc_size=sizeof(filter);filter.dbcc_devicetype=DBT_DEVTYP_DEVICEINTERFACE;filter.dbcc_classguid=guid;deviceNotice=RegisterDeviceNotificationW(window,&filter,DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    Device::stop=stopEvent;Device::target=window;Device::message=ResultMessage;
    Device::initial=status;
    Device::logDirectory=directory+L"\\batteryhistory";
    Device::refresh=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    deviceOwnerMutex=CreateMutexW(nullptr,FALSE,Handoff::MutexName(directory).c_str());
    if(!Device::refresh||!deviceOwnerMutex||!StartDevice()){
        MessageBoxW(window,Localization::Get(Localization::T284),DisplayName,MB_OK|MB_ICONERROR);
        DestroyWindow(window);
    }
    else SetTimer(window,RefreshTimer,Period,nullptr);
    MSG message;
    while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    ProgramLog::Write("program_stopped");
    if(trayIcon)DestroyIcon(trayIcon);if(deviceOwnerMutex)CloseHandle(deviceOwnerMutex);if(Device::refresh)CloseHandle(Device::refresh);CloseHandle(stopEvent);CloseHandle(mutexHandle);CoUninitialize();return 0;
}
