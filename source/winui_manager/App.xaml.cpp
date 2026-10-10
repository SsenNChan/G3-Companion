#include "pch.h"
#include "../localization.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include "LocalizationResources.h"

namespace
{
    LRESULT CALLBACK ManagerSizing(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR subclassId,DWORD_PTR reference)
    {
        if(message==WM_GETMINMAXINFO)
        {
            auto limits=reinterpret_cast<MINMAXINFO*>(lparam);
            const UINT dpi=GetDpiForWindow(hwnd);
            limits->ptMinTrackSize={MulDiv(640,dpi,96),MulDiv(480,dpi,96)};
            return 0;
        }
        auto size=reinterpret_cast<ManagerWindowSize::State*>(reference);
        if(message==WM_SIZE && wparam==SIZE_RESTORED && !IsZoomed(hwnd) && !IsIconic(hwnd)){
            RECT bounds{};
            if(GetWindowRect(hwnd,&bounds))ManagerWindowSize::Remember(*size,{bounds.right-bounds.left,bounds.bottom-bounds.top},GetDpiForWindow(hwnd));
        }
        if(message==WM_EXITSIZEMOVE)ManagerWindowSize::Save(*size);
        if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,ManagerSizing,subclassId);
        return DefSubclassProc(hwnd,message,wparam,lparam);
    }
}

namespace winrt::G3Companion::implementation
{
    App::App(){InitializeComponent();}
    void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        try {
        Microsoft::UI::Xaml::ResourceDictionary resources;
        Resources(resources);
        Localization::AddXamlResources(resources);
        resources.MergedDictionaries().Append(Microsoft::UI::Xaml::Controls::XamlControlsResources());
        for(const auto source:{L"CompactColorPicker.xaml",L"PowerSwitchIllustration.xaml",L"ApplicationIcon.xaml",L"ApplicationStyles.xaml"}){
            Microsoft::UI::Xaml::ResourceDictionary dictionary;
            dictionary.Source(Windows::Foundation::Uri(L"ms-appx:///"+std::wstring(source)));
            resources.MergedDictionaries().Append(dictionary);
        }
        auto main=winrt::make_self<MainWindow>();
        window=main.as<Microsoft::UI::Xaml::Window>();
        deviceController=std::make_unique<DeviceController>();
        main->SetApplyHandler([this](auto const& draft,auto const& baseline,auto mode){deviceController->Apply(draft,baseline,mode);});
        main->SetRefreshHandler([this](){deviceController->Refresh();});
        main->SetCloseHandler([this](){ManagerWindowSize::Save(windowSize);deviceController->Stop();});
        HWND hwnd=nullptr;check_hresult(window.as<::IWindowNative>()->get_WindowHandle(&hwnd));
        Monitoring::Paths monitoringPaths;
        check_hresult(Monitoring::CurrentPaths(monitoringPaths));
        main->ConfigureMonitoring(monitoringPaths);
        {
            HRESULT monitorResult=Monitoring::SynchronizeStartup(monitoringPaths);
            if(FAILED(monitorResult))MessageBoxW(hwnd,Localization::Get(Localization::T290),L"G3 Companion",MB_OK|MB_ICONERROR);
        }
        wchar_t module[32768]{};
        const DWORD length=GetModuleFileNameW(nullptr,module,ARRAYSIZE(module));
        check_bool(length && length<ARRAYSIZE(module));
        windowSize.settingsPath.assign(module,length);
        windowSize.settingsPath.resize(windowSize.settingsPath.find_last_of(L"\\/")+1);
        windowSize.settingsPath+=L"settings-0.0.7-alpha.ini";
        ManagerWindowSize::Load(windowSize);
        if(!SetWindowSubclass(hwnd,ManagerSizing,1,reinterpret_cast<DWORD_PTR>(&windowSize)))throw_last_error();
        MONITORINFO monitor{sizeof(monitor)};
        check_bool(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor));
        const auto size=ManagerWindowSize::FitWorkArea(windowSize,monitor.rcWork,GetDpiForWindow(hwnd));
        window.AppWindow().Resize({size.cx,size.cy});
        RECT bounds{};
        if(GetWindowRect(hwnd,&bounds)){
            const auto left=(std::min)((std::max)(bounds.left,monitor.rcWork.left),
                (std::max)(monitor.rcWork.left,monitor.rcWork.right-(bounds.right-bounds.left)));
            const auto top=(std::min)((std::max)(bounds.top,monitor.rcWork.top),
                (std::max)(monitor.rcWork.top,monitor.rcWork.bottom-(bounds.bottom-bounds.top)));
            window.AppWindow().Move({left,top});
        }
        window.Activate();
        windowSize.enabled=true;
        auto weak=main->get_weak();
        if(!deviceController->Start(hwnd,[weak](auto const& snapshot){if(auto target=weak.get())target->UpdateSnapshot(snapshot);})){
            DWORD error=GetLastError();
            if(error!=ERROR_ALREADY_EXISTS){
                std::wstring text=error==ERROR_REVISION_MISMATCH?Localization::Get(Localization::T291):Localization::Format(Localization::T292,{{L"errorCode",std::to_wstring(error)}});
                MessageBoxW(hwnd,text.c_str(),L"G3 Companion",MB_OK|MB_ICONERROR);
            }
            window.Close();
        }
        else if(Monitoring::Read(monitoringPaths).enabled){
            HRESULT monitorResult=Monitoring::EnsureTray(monitoringPaths);
            if(FAILED(monitorResult))MessageBoxW(hwnd,Localization::Get(Localization::T293),L"G3 Companion",MB_OK|MB_ICONERROR);
        }
        } catch(hresult_error const& error) {
            const auto message=Localization::Format(Localization::T294,{{L"details",error.message().c_str()}});
            MessageBoxW(nullptr,message.c_str(),L"G3 Companion",MB_OK|MB_ICONERROR);
            Exit();
        }
    }
}
